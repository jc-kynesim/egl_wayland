// gcc -o test init_window.c -I. -lwayland-client -lwayland-server -lwayland-client-protocol -lwayland-egl -lEGL -lGLESv2
#include <wayland-client-core.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h> // Wayland EGL MUST be included before EGL headers

#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include "init_window.h"
#include "dmabuf_alloc.h"
//#include "log.h"
#define LOG printf

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <math.h>

#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/time.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include <libdrm/drm_fourcc.h>

#include <pthread.h>
#include <semaphore.h>

#define TRACE_ALL 1
#define  DEBUG_SOLID 0

#define DBCOUNT 32
#define BUF_W 640
#define BUF_STRIDE ((BUF_W + 127) & ~127)
#define BUF_H 480
#define BUF_STRIDE2 (BUF_H * 3/2)
#define BUFSIZE (BUF_STRIDE * BUF_STRIDE2)
#define BAR_Y 0xff
#define BKG_Y 0


#define ES_SIG 0x12345678

#define W_SUBSURFACE 0

struct wl_egl_window *egl_window;
struct wl_region *region;

struct xdg_wm_base *XDGWMBase;
struct xdg_surface *XDGSurface;
struct xdg_toplevel *XDGToplevel;

struct _escontext
{
	struct wl_display *native_display;
	int window_width;
	int window_height;
	int req_w;
	int req_h;
	struct wl_egl_window *native_window;
	struct wl_compositor *w_compositor;
	struct wl_surface *w_surface;
	struct wl_surface *w_surface2;
	struct wl_subsurface *w_subsurface2;
	struct zwp_linux_dmabuf_v1 * linux_dmabuf_v1_bind;
	struct wl_shm *w_shm;
	struct wl_subcompositor *w_subcompositor;
	struct zxdg_decoration_manager_v1 *x_decoration;
	struct wp_viewporter *w_viewporter;
	struct wp_viewport *w_viewport;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;

	unsigned int sig;
};

struct _escontext ESContext = {
	.native_display = NULL,
	.window_width = 0,
	.window_height = 0,
	.native_window  = 0,
	.display = NULL,
	.context = NULL,
	.surface = NULL,
	.sig = ES_SIG
};

typedef struct egl_aux_s
{
	int fd;
	GLuint texture;
} egl_aux_t;


#define THING_Q_LEN 128
typedef struct thing_q_ss {
	pthread_mutex_t lock;
	unsigned int p_in;
	unsigned int p_out;
	void * q[THING_Q_LEN];
} thing_q;


#define FQLEN 128

struct egl_wayland_out_env
{
	struct _escontext * es;

	int show_all;
	int window_width, window_height;
	int window_x, window_y;
	int fullscreen;

	uint64_t last_display;

	egl_aux_t aux[32];

	pthread_t q_thread;
	sem_t display_start_sem;
	sem_t free_sem;
	int prod_fd;
	int q_terminate;
	bool is_egl;

	thing_q q_in;
	thing_q q_out;
};

#define TRUE 1
#define FALSE 0

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

bool program_alive;


static inline char drmu_log_safechar(int c)
{
    return (c < ' ' || c >=0x7f) ? '?' : c;
}

static inline const char * drmu_log_fourcc_to_str(char buf[5], uint32_t fcc)
{
    if (fcc == 0)
        return "----";
    buf[0] = drmu_log_safechar((fcc >> 0) & 0xff);
    buf[1] = drmu_log_safechar((fcc >> 8) & 0xff);
    buf[2] = drmu_log_safechar((fcc >> 16) & 0xff);
    buf[3] = drmu_log_safechar((fcc >> 24) & 0xff);
    buf[4] = 0;
    return buf;
}

#define drmu_log_fourcc(fcc) drmu_log_fourcc_to_str((char[5]){0}, fcc)

static uint64_t us_time()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_nsec / 1000) + ((uint64_t)ts.tv_sec * 1000000);
}

/* Shared memory support code */
static void
randname(char *buf)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

static int
create_shm_file(void)
{
	int retries = 100;
	do {
		char name[] = "/wl_shm-XXXXXX";
		randname(name + sizeof(name) - 7);
		--retries;
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);
	return -1;
}

static int
allocate_shm_file(size_t size)
{
	int fd = create_shm_file();
	if (fd < 0)
		return -1;
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void
shm_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	(void)data;
	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener shm_buffer_listener = {
	.release = shm_buffer_release,
};

static void
q_thing(thing_q * const tq, void * const v)
{
	pthread_mutex_lock(&tq->lock);
	tq->q[tq->p_in] = v;
	tq->p_in = (tq->p_in + 1) & (THING_Q_LEN - 1);
	assert(tq->p_in != tq->p_out);
	pthread_mutex_unlock(&tq->lock);
}

static void *
dq_thing(thing_q * const tq)
{
	void * rv = NULL;

	pthread_mutex_lock(&tq->lock);
	if (tq->p_in != tq->p_out) {
		rv = tq->q[tq->p_out];
		tq->q[tq->p_out] = NULL;
		tq->p_out = (tq->p_out + 1) & (THING_Q_LEN - 1);
	}
	pthread_mutex_unlock(&tq->lock);

	return rv;
}

static void
thing_q_init(thing_q * const tq)
{
	memset(tq, 0, sizeof(tq));
	pthread_mutex_init(&tq->lock, NULL);
}

static void
thing_q_uninit(thing_q * const tq)
{
	pthread_mutex_destroy(&tq->lock);
}



static struct wl_buffer *
draw_frame(struct _escontext * const es)
{
	const int width = 640, height = 480;
	int stride = width * 4;
	int size = stride * height;

	int fd = allocate_shm_file(size);
	if (fd == -1) {
		return NULL;
	}

	uint32_t *data = mmap(NULL, size,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(es->w_shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
			width, height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Draw checkerboxed background */
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if ((x + y / 8 * 8) % 16 < 8)
				data[y * width + x] = 0xFF666666;
			else
				data[y * width + x] = 0xFFEEEEEE;
		}
	}

	munmap(data, size);
	wl_buffer_add_listener(buffer, &shm_buffer_listener, NULL);
	return buffer;
}

struct dmabuf_w_env_s {
	struct dmabuf_h * buf;
	struct egl_wayland_out_env * de;
};

static struct dmabuf_w_env_s *
dmabuf_w_env_new(struct egl_wayland_out_env *const de, struct dmabuf_h *const buf)
{
	struct dmabuf_w_env_s * const dbe = malloc(sizeof(*dbe));
	if (!dbe)
		return NULL;
	dbe->buf = buf;
	dbe->de = de;
	return dbe;
}

static void
dmabuf_w_env_delete(struct dmabuf_w_env_s * const dbe)
{
	q_thing(&dbe->de->q_out, dbe->buf);
	sem_post(&dbe->de->free_sem);
	free(dbe);
}

static void
w_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	struct dmabuf_w_env_s * const dbe = data;

	/* Sent by the compositor when it's no longer using this buffer */
	wl_buffer_destroy(wl_buffer);
	dmabuf_w_env_delete(dbe);
}

static const struct wl_buffer_listener w_buffer_listener = {
	.release = w_buffer_release,
};

static void
create_wl_dmabuf_succeeded(void *data, struct zwp_linux_buffer_params_v1 *params,
		 struct wl_buffer *new_buffer)
{
	struct dmabuf_w_env_s * const dbe = data;
	struct _escontext * const es = dbe->de->es;
	struct dmabuf_h * buf = dbe->buf;
#if 0
	ConstructBufferData *d = data;

	g_mutex_lock(&d->lock);
	d->wbuf = new_buffer;
	g_cond_signal(&d->cond);
	g_mutex_unlock(&d->lock);
#endif
	printf("%s: ok data=%p, buf=%p, es=%p, %dx%d\n", __func__, data, (void*)buf, (void*)es, es->req_w, es->req_h);
	fflush(stdout);
	zwp_linux_buffer_params_v1_destroy(params);

	wl_buffer_add_listener(new_buffer, &w_buffer_listener, dbe);

	// *************
	assert(es->sig == ES_SIG);
	if (0)
	{
	wl_buffer_destroy(new_buffer);
	dmabuf_w_env_delete(dbe);
	new_buffer = draw_frame(es);
	}
	// *************

//	wl_surface_attach(es->w_surface2, draw_frame(es), 0, 0);
//	wl_surface_damage(es->w_surface2, 0, 0, INT32_MAX, INT32_MAX);
//    wl_surface_commit(es->w_surface2);

	wl_surface_attach(es->w_surface, new_buffer, 0, 0);
	wp_viewport_set_destination(es->w_viewport, es->req_w, es->req_h);
	wl_surface_damage(es->w_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(es->w_surface);
}

static void
create_wl_dmabuf_failed(void *data, struct zwp_linux_buffer_params_v1 *params)
{
	struct dmabuf_w_env_s * const dbe = data;
#if 0
	ConstructBufferData *d = data;

	g_mutex_lock(&d->lock);
	d->wbuf = NULL;
	g_cond_signal(&d->cond);
	g_mutex_unlock(&d->lock);
#endif
	(void)data;
	printf("%s: FAILED\n", __func__);
	zwp_linux_buffer_params_v1_destroy(params);
	dmabuf_w_env_delete(dbe);
}

static const struct zwp_linux_buffer_params_v1_listener params_wl_dmabuf_listener = {
	create_wl_dmabuf_succeeded,
	create_wl_dmabuf_failed
};

static struct wl_buffer*
do_display_dmabuf(egl_wayland_out_env_t *const de, struct dmabuf_h * const buf)
{
	struct _escontext *const es = de->es;
	struct zwp_linux_buffer_params_v1 *params;
	const uint32_t format = DRM_FORMAT_NV12;
	const unsigned int width = BUF_W;
	const unsigned int height = BUF_H;
	const uint64_t mod = DRM_FORMAT_MOD_BROADCOM_SAND128_COL_HEIGHT(BUF_STRIDE2);

	LOG("<<< %s\n", __func__);

	/* Creation and configuration of planes  */
	params = zwp_linux_dmabuf_v1_create_params(es->linux_dmabuf_v1_bind);
	if (!params)
	{
		LOG("zwp_linux_dmabuf_v1_create_params FAILED\n");
		return NULL;
	}

	zwp_linux_buffer_params_v1_add(params, dmabuf_fd(buf), 0, 0, BUF_STRIDE,
								   (unsigned int)(mod >> 32), (unsigned int)(mod & 0xFFFFFFFF));
	zwp_linux_buffer_params_v1_add(params, dmabuf_fd(buf), 1, BUF_H * 128, BUF_STRIDE,
								   (unsigned int)(mod >> 32), (unsigned int)(mod & 0xFFFFFFFF));

	assert(es->sig == ES_SIG);

	/* Request buffer creation */
	zwp_linux_buffer_params_v1_add_listener(params, &params_wl_dmabuf_listener,
		dmabuf_w_env_new(de, buf));

	zwp_linux_buffer_params_v1_create(params, width, height, format, 0);

	wl_display_flush(es->native_display);

#if 0
	/* Wait for the request answer */
	wl_display_flush(gst_wl_display_get_display(display));
	data.wbuf = (gpointer)0x1;
	timeout = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
	while (data.wbuf == (gpointer)0x1) {
		if (!g_cond_wait_until(&data.cond, &data.lock, timeout)) {
			GST_ERROR_OBJECT(mem->allocator, "zwp_linux_buffer_params_v1 time out");
			zwp_linux_buffer_params_v1_destroy(params);
			data.wbuf = NULL;
		}
	}

out:
	if (!data.wbuf) {
		GST_ERROR_OBJECT(mem->allocator, "can't create linux-dmabuf buffer");
	} else {
		GST_DEBUG_OBJECT(mem->allocator, "created linux_dmabuf wl_buffer (%p):"
				 "%dx%d, fmt=%.4s, %d planes",
				 data.wbuf, width, height, (char *)&format, nplanes);
	}

	g_mutex_unlock(&data.lock);
	g_mutex_clear(&data.lock);
	g_cond_clear(&data.cond);

	return data.wbuf;
#endif
	return NULL;
}

static int do_display(egl_wayland_out_env_t *const de, struct _escontext *const es, struct dmabuf_h * const buf)
{
	(void)de;
	(void)es;
	(void)buf;
#if 0
#if DEBUG_SOLID
	(void)de;
	(void)frame;
	static double a = 0.3;

	glClearColor(0.5, a, 0.0, 1.0);

	a += 0.05;
	if (a >= 1.0)
		a = 0.0;

	glClear(GL_COLOR_BUFFER_BIT);

	eglSwapBuffers(es->display, es->surface);
#else
	const AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
	egl_aux_t *da = NULL;
	unsigned int i;

#if TRACE_ALL
	LOG("<<< %s\n", __func__);
#endif

	{
		static int z = 0;
		if (!z)
		{
			z = 1;
			if (!check_support(es, desc->layers[0].format, desc->objects[0].format_modifier))
			{
				LOG("No support for format\n");
				return -1;
			}
		}
	}

	if (es->req_w != es->window_width || es->req_h != es->window_height) {
		LOG("%s: Resize %dx%d -> %dx%d\n", __func__, es->window_width, es->window_height, es->req_w, es->req_h);
		wl_egl_window_resize(es->native_window, es->req_w, es->req_h, 0, 0);
		es->window_width = es->req_w;
		es->window_height = es->req_h;
	}

	for (i = 0; i != 32; ++i)
	{
		if (de->aux[i].fd == -1 || de->aux[i].fd == desc->objects[0].fd)
		{
			da = de->aux + i;
			break;
		}
	}

	if (da == NULL)
	{
		LOG("%s: Out of handles\n", __func__);
		return AVERROR(EINVAL);
	}

	if (da->texture == 0)
	{
		EGLint attribs[50];
		EGLint *a = attribs;
		int i, j;
		static const EGLint anames[] = {
			EGL_DMA_BUF_PLANE0_FD_EXT,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT,
			EGL_DMA_BUF_PLANE0_PITCH_EXT,
			EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
			EGL_DMA_BUF_PLANE1_FD_EXT,
			EGL_DMA_BUF_PLANE1_OFFSET_EXT,
			EGL_DMA_BUF_PLANE1_PITCH_EXT,
			EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
			EGL_DMA_BUF_PLANE2_FD_EXT,
			EGL_DMA_BUF_PLANE2_OFFSET_EXT,
			EGL_DMA_BUF_PLANE2_PITCH_EXT,
			EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
			EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
		};
		const EGLint *b = anames;

		*a++ = EGL_WIDTH;
		*a++ = av_frame_cropped_width(frame);
		*a++ = EGL_HEIGHT;
		*a++ = av_frame_cropped_height(frame);
		*a++ = EGL_LINUX_DRM_FOURCC_EXT;
		*a++ = desc->layers[0].format;

		for (i = 0; i < desc->nb_layers; ++i)
		{
			for (j = 0; j < desc->layers[i].nb_planes; ++j)
			{
				const AVDRMPlaneDescriptor *const p = desc->layers[i].planes + j;
				const AVDRMObjectDescriptor *const obj = desc->objects + p->object_index;
				*a++ = *b++;
				*a++ = obj->fd;
				*a++ = *b++;
				*a++ = p->offset;
				*a++ = *b++;
				*a++ = p->pitch;
				if (obj->format_modifier == 0)
				{
					b += 2;
				}
				else
				{
					*a++ = *b++;
					*a++ = (EGLint)(obj->format_modifier & 0xFFFFFFFF);
					*a++ = *b++;
					*a++ = (EGLint)(obj->format_modifier >> 32);
				}
			}
		}

		*a = EGL_NONE;

#if TRACE_ALL
		for (a = attribs, i = 0; *a != EGL_NONE; a += 2, ++i)
		{
			LOG("[%2d] %4x: %d\n", i, a[0], a[1]);
		}
#endif
		{
			const EGLImage image = eglCreateImageKHR(es->display,
													 EGL_NO_CONTEXT,
													 EGL_LINUX_DMA_BUF_EXT,
													 NULL, attribs);
			if (!image)
			{
				LOG("Failed to import fd %d\n", desc->objects[0].fd);
				return -1;
			}

			glGenTextures(1, &da->texture);
			glBindTexture(GL_TEXTURE_EXTERNAL_OES, da->texture);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

			eglDestroyImageKHR(es->display, image);
		}

		da->fd = desc->objects[0].fd;

#if 0
		LOG( "%dx%d, fmt: %x, boh=%d,%d,%d,%d, pitch=%d,%d,%d,%d,"
			" offset=%d,%d,%d,%d, mod=%llx,%llx,%llx,%llx\n",
			av_frame_cropped_width(frame),
			av_frame_cropped_height(frame),
			desc->layers[0].format,
			bo_plane_handles[0],
			bo_plane_handles[1],
			bo_plane_handles[2],
			bo_plane_handles[3],
			pitches[0],
			pitches[1],
			pitches[2],
			pitches[3],
			offsets[0],
			offsets[1],
			offsets[2],
			offsets[3],
			(long long)modifiers[0],
			(long long)modifiers[1],
			(long long)modifiers[2],
			(long long)modifiers[3]
		   );
#endif
	}

	glClearColor(0.5, 0.5, 0.5, 0.5);
	glClear(GL_COLOR_BUFFER_BIT);

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, da->texture);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	eglSwapBuffers(es->display, es->surface);

	glDeleteTextures(1, &da->texture);
	da->texture = 0;
	da->fd = -1;
#endif
#endif
	return 0;
}


static GLint
compile_shader(GLenum target, const char *source)
{
	GLuint s = glCreateShader(target);

	if (s == 0)
	{
		LOG("Failed to create shader\n");
		return 0;
	}

	glShaderSource(s, 1, (const GLchar **)&source, NULL);
	glCompileShader(s);

	{
		GLint ok;
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

		if (!ok)
		{
			GLchar *info;
			GLint size;

			glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
			info = malloc(size);

			glGetShaderInfoLog(s, size, NULL, info);
			LOG("Failed to compile shader: %ssource:\n%s\n", info, source);

			return 0;
		}
	}

	return s;
}

static GLuint link_program(GLint vs, GLint fs)
{
	GLuint prog = glCreateProgram();

	if (prog == 0)
	{
		LOG("Failed to create program\n");
		return 0;
	}

	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);

	{
		GLint ok;
		glGetProgramiv(prog, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			/* Some drivers return a size of 1 for an empty log.  This is the size
			 * of a log that contains only a terminating NUL character.
			 */
			GLint size;
			GLchar *info = NULL;
			glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
			if (size > 1)
			{
				info = malloc(size);
				glGetProgramInfoLog(prog, size, NULL, info);
			}

			LOG("Failed to link: %s\n",
				(info != NULL) ? info : "<empty log>");
			return 0;
		}
	}

	return prog;
}

static int
gl_setup()
{
	const char *vs =
		"attribute vec4 pos;\n"
		"varying vec2 texcoord;\n"
		"\n"
		"void main() {\n"
		"  gl_Position = pos;\n"
		"  texcoord.x = (pos.x + 1.0) / 2.0;\n"
		"  texcoord.y = (-pos.y + 1.0) / 2.0;\n"
		"}\n";
	const char *fs =
		"#extension GL_OES_EGL_image_external : enable\n"
		"precision mediump float;\n"
		"uniform samplerExternalOES s;\n"
		"varying vec2 texcoord;\n"
		"void main() {\n"
		"  gl_FragColor = texture2D(s, texcoord);\n"
		"}\n";

	GLuint vs_s;
	GLuint fs_s;
	GLuint prog;

	if (!(vs_s = compile_shader(GL_VERTEX_SHADER, vs)) ||
		!(fs_s = compile_shader(GL_FRAGMENT_SHADER, fs)) ||
		!(prog = link_program(vs_s, fs_s)))
		return -1;

	glUseProgram(prog);

	{
		static const float verts[] = {
			-1, -1,
			1, -1,
			1, 1,
			-1, 1,
		};
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
	}

	glEnableVertexAttribArray(0);
	return 0;
}

#define PACE_TIME 10000

static void* display_thread(void *v)
{
	egl_wayland_out_env_t *const de = v;
	struct _escontext *const es = &ESContext;
	struct pollfd pollfds[2];
	int wl_fd;
	int wl_poll_out = 0;

#if TRACE_ALL
	LOG("<<< %s\n", __func__);
#endif

	if (de->is_egl)
	{
		// Make the context current
		if (!eglMakeCurrent(es->display, es->surface, es->surface, es->context))
		{
			LOG("Could not make the current window current !\n");
			goto fail;
		}

		LOG("GL Vendor: %s\n", glGetString(GL_VENDOR));
		LOG("GL Version: %s\n", glGetString(GL_VERSION));
		LOG("GL Renderer: %s\n", glGetString(GL_RENDERER));
		LOG("GL Extensions: %s\n", glGetString(GL_EXTENSIONS));
		LOG("EGL Extensions: %s\n", eglQueryString(es->display, EGL_EXTENSIONS));

		if (!epoxy_has_egl_extension(es->display, "EGL_EXT_image_dma_buf_import"))
		{
			LOG("Missing EGL EXT image dma_buf extension\n");
			goto fail;
		}

		if (gl_setup())
		{
			LOG("%s: gl_setup failed\n", __func__);
			goto fail;
		}

		{
			EGLint fmts[128];
			EGLint fcount = 0;
			EGLint i;

			eglQueryDmaBufFormatsEXT(es->display, 128, fmts, &fcount);
			LOG("DmaBuf formats found=%d\n", fcount);
			for (i = 0; i != fcount; ++i)
			{
				LOG("[%d] %s\n", i, drmu_log_fourcc(fmts[i]));
			}
		}
	}

#if TRACE_ALL
	LOG("--- %s: Start done\n", __func__);
#endif
	wl_fd = wl_display_get_fd(es->native_display);

	while (!de->q_terminate)
	{
		int rv;
		uint64_t now;

		for(;;) {
			wl_display_dispatch_pending(es->native_display);
			if (wl_display_prepare_read(es->native_display) == 0)
				break;
			if (errno != EAGAIN)
			{
				LOG("prepared_read: %s\n", strerror(errno));
				break;
			}
		}

		wl_poll_out = 0;
		if (wl_display_flush(es->native_display) == -1 && errno == EAGAIN)
			wl_poll_out = 1;

		pollfds[0] = (struct pollfd){.fd = de->prod_fd, .events = POLLIN};
		pollfds[1] = (struct pollfd){.fd = wl_fd, .events = wl_poll_out ? POLLIN | POLLOUT : POLLIN};

justpoll:
		do {
			rv = poll(pollfds, 2, 10);
		} while (rv < 0 && errno == EINTR);

		if (rv < 0)
		{
			LOG("Poll failed: %s\n", strerror(errno));
			break;
		}

		now = us_time();

		if (now - de->last_display >= PACE_TIME) {
			struct dmabuf_h * buf;

//			printf("now=%"PRId64", last=%"PRId64", diff=%"PRId64"\n", now, de->last_display, now - de->last_display);

			if (now - de->last_display > 1000000)
				de->last_display = now;
			else
				de->last_display += PACE_TIME;

			buf = dq_thing(&de->q_in);

			if (buf)
			{
				if (de->is_egl)
					do_display(de, es, buf);
				else
					do_display_dmabuf(de, buf);
			}
		}

		if (rv == 0)
		{
//			LOG("Poll unexpected timeout\n");
			goto justpoll;
		}

		if (pollfds[0].revents)
		{
			uint64_t rcount = 0;

			if (read(de->prod_fd, &rcount, sizeof(rcount)) != sizeof(rcount))
				LOG("Unexpected prod read\n");
		}

		if (pollfds[1].revents)
		{
			if (wl_display_read_events(es->native_display) != 0)
				LOG("Read Event Failed\n");
		}
		else
			goto justpoll;
	}

#if TRACE_ALL
	LOG(">>> %s\n", __func__);
#endif

	return NULL;

fail:
#if TRACE_ALL
	LOG(">>> %s: FAIL\n", __func__);
#endif
	de->q_terminate = 1;
	sem_post(&de->display_start_sem);

	return NULL;
}

static void xdg_toplevel_handle_configure(void *data,
	struct xdg_toplevel *xdg_toplevel, int32_t w, int32_t h,
	struct wl_array *states)
{
	struct _escontext * const es = data;
	(void)xdg_toplevel;
	uint32_t * p;

	LOG("%s: %dx%d\n", __func__, w, h);

	wl_array_for_each(p, states) {
		LOG("  [] %"PRId32"\n", *p);
	}

	// no window geometry event, ignore
	if (w == 0 && h == 0)
		return;

	es->req_h = h;
	es->req_w = w;
}

static void xdg_toplevel_handle_close(void *data,
									  struct xdg_toplevel *xdg_toplevel)
{
	(void)data;
	(void)xdg_toplevel;

	// window closed, be sure that this event gets processed
	program_alive = false;
}

static void
xdg_toplevel_configure_bounds_cb(void *data,
			 struct xdg_toplevel *xdg_toplevel,
			 int32_t width,
			 int32_t height)
{
	(void)data;
	LOG("%s[%p]: %dx%d\n", __func__, (void*)xdg_toplevel, width, height);
}

/**
 * compositor capabilities
 *
 * This event advertises the capabilities supported by the
 * compositor. If a capability isn't supported, clients should hide
 * or disable the UI elements that expose this functionality. For
 * instance, if the compositor doesn't advertise support for
 * minimized toplevels, a button triggering the set_minimized
 * request should not be displayed.
 *
 * The compositor will ignore requests it doesn't support. For
 * instance, a compositor which doesn't advertise support for
 * minimized will ignore set_minimized requests.
 *
 * Compositors must send this event once before the first
 * xdg_surface.configure event. When the capabilities change,
 * compositors must send this event again and then send an
 * xdg_surface.configure event.
 *
 * The configured state should not be applied immediately. See
 * xdg_surface.configure for details.
 *
 * The capabilities are sent as an array of 32-bit unsigned
 * integers in native endianness.
 * @param capabilities array of 32-bit capabilities
 * @since 5
 */
static void xdg_toplevel_wm_capabilities_cb(void *data,
			struct xdg_toplevel *xdg_toplevel,
			struct wl_array *capabilities)
{
	(void)data;
	LOG("%s[%p]:\n", __func__, (void*)xdg_toplevel);
	for (size_t i = 0; i != capabilities->size / 4; ++i) {
		uint32_t cap = ((const uint32_t *)capabilities->data)[i];
		LOG("  [%zd]: %"PRId32"\n", i, cap);
	}
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
	.configure_bounds = xdg_toplevel_configure_bounds_cb,
	.wm_capabilities = xdg_toplevel_wm_capabilities_cb,
};


static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
								  uint32_t serial)
{
	struct egl_wayland_out_env * const de = data;
//	struct _escontext *const es = de->es;
	LOG("%s\n", __func__);
	// confirm that you exist to the compositor
	xdg_surface_ack_configure(xdg_surface, serial);

	// ********************
//	struct wl_buffer *buffer = draw_frame(es);
//	wl_surface_attach(es->w_surface, buffer, 0, 0);
//	wl_surface_commit(es->w_surface);
	// ********************

	sem_post(&de->display_start_sem);  //***********
}

const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base,
							 uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(xdg_wm_base, serial);
}

const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

void CreateNativeWindow(struct _escontext * const es, char *title)
{
	(void)title;

	region = wl_compositor_create_region(es->w_compositor);

	wl_region_add(region, 0, 0, es->req_w, es->req_h);
	wl_surface_set_opaque_region(es->w_surface, region);

	LOG("%s: %dx%d\n", __func__, es->req_w, es->req_h);
	es->window_width = es->req_w;
	es->window_height = es->req_h;
}

EGLBoolean CreateEGLContext(struct _escontext * const es)
{
	EGLint numConfigs;
	EGLint majorVersion;
	EGLint minorVersion;
	EGLContext context;
	EGLSurface surface;
	EGLConfig config;
	EGLint fbAttribs[] =
	{
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE,   	 8,
		EGL_GREEN_SIZE, 	 8,
		EGL_BLUE_SIZE,  	 8,
		EGL_NONE
	};
	EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE, EGL_NONE };
	EGLDisplay display = eglGetDisplay(es->native_display);
	if (display == EGL_NO_DISPLAY)
	{
		LOG("No EGL Display...\n");
		return EGL_FALSE;
	}

	// Initialize EGL
	if (!eglInitialize(display, &majorVersion, &minorVersion))
	{
		LOG("No Initialisation...\n");
		return EGL_FALSE;
	}

	LOG("EGL init: version %d.%d\n", majorVersion, minorVersion);

	eglBindAPI(EGL_OPENGL_ES_API);

	// Get configs
	if ((eglGetConfigs(display, NULL, 0, &numConfigs) != EGL_TRUE) || (numConfigs == 0))
	{
		LOG("No configuration...\n");
		return EGL_FALSE;
	}
	LOG("GL Configs: %d\n", numConfigs);

	// Choose config
	if ((eglChooseConfig(display, fbAttribs, &config, 1, &numConfigs) != EGL_TRUE) || (numConfigs != 1))
	{
		LOG("No configuration...\n");
		return EGL_FALSE;
	}

	es->native_window =
		wl_egl_window_create(es->w_surface, es->window_width, es->window_height);

	if (es->native_window == EGL_NO_SURFACE)
	{
		LOG("No window !?\n");
		return EGL_FALSE;
	}
	else
		LOG("Window created !\n");

	// Create a surface
	surface = eglCreateWindowSurface(display, config, es->native_window, NULL);
	if (surface == EGL_NO_SURFACE)
	{
		LOG("No surface...\n");
		return EGL_FALSE;
	}

	// Create a GL context
	context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs);
	if (context == EGL_NO_CONTEXT)
	{
		LOG("No context...\n");
		return EGL_FALSE;
	}

	es->display = display;
	es->surface = surface;
	es->context = context;
	return EGL_TRUE;
}

static EGLBoolean CreateWindowWithEGLContext(struct _escontext * const es, char *title)
{
	CreateNativeWindow(es, title);
	return CreateEGLContext(es);
}

static void CreateWindowForDmaBuf(struct _escontext * const es, char *title)
{
	CreateNativeWindow(es, title);
}

unsigned long last_click = 0;
void RefreshWindow()
{
	eglSwapBuffers(ESContext.display, ESContext.surface);
}

static void linux_dmabuf_v1_listener_format(void *data,
			   struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
			   uint32_t format)
{
	// Superceeded by _modifier
	struct _escontext * const es = data;
	(void)zwp_linux_dmabuf_v1;
	(void)format;
	printf("%s[%p], %s\n", __func__, (void*)es, drmu_log_fourcc(format));
}

static void
linux_dmabuf_v1_listener_modifier(void *data,
		 struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
		 uint32_t format,
		 uint32_t modifier_hi,
		 uint32_t modifier_lo)
{
	struct _escontext * const es = data;
	(void)zwp_linux_dmabuf_v1;

	printf("%s[%p], %s %08x%08x\n", __func__, (void*)es, drmu_log_fourcc(format), modifier_hi, modifier_lo);
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_v1_listener = {
	.format = linux_dmabuf_v1_listener_format,
	.modifier = linux_dmabuf_v1_listener_modifier,
};

static void
decoration_configure(void *data,
			  struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1,
			  uint32_t mode)
{
	(void)data;
	printf("%s[%p]: mode %d\n", __func__, (void*)zxdg_toplevel_decoration_v1, mode);
	zxdg_toplevel_decoration_v1_destroy(zxdg_toplevel_decoration_v1);
}

static struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
	.configure = decoration_configure,
};

static void global_registry_handler(void *data, struct wl_registry *registry, uint32_t id,
									const char *interface, uint32_t version)
{
	struct _escontext * const es = data;
	(void)version;

	LOG("Got a registry event for %s id %d\n", interface, id);
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		es->w_compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
	if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		es->linux_dmabuf_v1_bind = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 1);
		zwp_linux_dmabuf_v1_add_listener(es->linux_dmabuf_v1_bind, &linux_dmabuf_v1_listener, es);
	}
	if (strcmp(interface, wl_shm_interface.name) == 0)
		es->w_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
	if (strcmp(interface, wl_subcompositor_interface.name) == 0)
		es->w_subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
	if (strcmp(interface, xdg_wm_base_interface.name) == 0)
	{
		XDGWMBase = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(XDGWMBase, &xdg_wm_base_listener, NULL);
	}
	if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
		es->x_decoration = wl_registry_bind(registry, id, &zxdg_decoration_manager_v1_interface, 1);
	if (strcmp(interface, wp_viewporter_interface.name) == 0)
		es->w_viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
}

static void global_registry_remover(void *data, struct wl_registry *registry, uint32_t id)
{
	(void)data;
	(void)registry;

	LOG("Got a registry losing event for %d\n", id);
}

const struct wl_registry_listener listener = {
	global_registry_handler,
	global_registry_remover
};

static void
get_server_references(struct _escontext * const es)
{

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL)
	{
		LOG("Can't connect to wayland display !?\n");
		exit(1);
	}
	LOG("Got a display !");

	struct wl_registry *wl_registry =
		wl_display_get_registry(display);
	wl_registry_add_listener(wl_registry, &listener, &ESContext);

	// This call the attached listener global_registry_handler
	wl_display_dispatch(display);
	wl_display_roundtrip(display);

	// If at this point, global_registry_handler didn't set the
	// compositor, nor the shell, bailout !
	if (es->w_compositor == NULL || XDGWMBase == NULL)
	{
		LOG("No compositor !? No XDG !! There's NOTHING in here !\n");
		exit(1);
	}
	else
	{
		LOG("Okay, we got a compositor and a shell... That's something !\n");
		es->native_display = display;
	}
}

void destroy_window(struct _escontext * const es)
{
	eglDestroySurface(es->display, es->surface);
	eglDestroyContext(es->display, es->context);
	wl_egl_window_destroy(es->native_window);
	xdg_toplevel_destroy(XDGToplevel);
	xdg_surface_destroy(XDGSurface);
	wl_surface_destroy(es->w_surface);
}

#if 0
int main()
{
	get_server_references();

	surface = wl_compositor_create_surface(compositor);
	if (surface == NULL)
	{
		LOG("No Compositor surface ! Yay....\n");
		exit(1);
	}
	else LOG("Got a compositor surface !\n");

	XDGSurface = xdg_wm_base_get_xdg_surface(XDGWMBase, surface);

	xdg_surface_add_listener(XDGSurface, &xdg_surface_listener, NULL);

	XDGToplevel = xdg_surface_get_toplevel(XDGSurface);
	xdg_toplevel_set_title(XDGToplevel, "Wayland EGL example");
	xdg_toplevel_add_listener(XDGToplevel, &xdg_toplevel_listener, NULL);

	wl_surface_commit(surface);

	CreateWindowWithEGLContext("Nya", 1280, 720);

	program_alive = true;

	while (program_alive)
	{
		wl_display_dispatch_pending(ESContext.native_display);
		draw();
		RefreshWindow();
	}

	destroy_window();
	wl_display_disconnect(ESContext.native_display);
	LOG("Display disconnected !\n");

	exit(0);
}
#endif

void egl_wayland_out_modeset(struct egl_wayland_out_env *dpo, int w, int h, AVRational frame_rate)
{
	(void)dpo;
	(void)w;
	(void)h;
	(void)frame_rate;
	/* NIF */
}

void
display_prod(struct egl_wayland_out_env *de)
{
	static const uint64_t one = 1;
	int rv;

	do {
		rv = write(de->prod_fd, &one, sizeof(one));
	} while (rv == -1 && errno == EINTR);

	if (rv != sizeof(one))
		LOG("Event prod failed!\n");
}


static struct egl_wayland_out_env*
wayland_out_new(const bool is_egl, const bool fullscreen)
{
	struct egl_wayland_out_env *de = calloc(1, sizeof(*de));
	unsigned int i;
	struct _escontext * const es = &ESContext;

	LOG("<<< %s\n", __func__);

	de->es = es;
	de->prod_fd = -1;
	de->q_terminate = 0;
	de->is_egl = is_egl;

	es->req_w = WINDOW_WIDTH;
	es->req_h = WINDOW_HEIGHT;

	sem_init(&de->display_start_sem, 0, 0);

	get_server_references(es);

	es->w_surface = wl_compositor_create_surface(es->w_compositor);
	if (es->w_surface == NULL)
	{
		LOG("No Compositor surface ! Yay....\n");
		exit(1);
	}
	else
		LOG("Got a compositor surface !\n");

	es->w_viewport = wp_viewporter_get_viewport(es->w_viewporter, es->w_surface);
	XDGSurface = xdg_wm_base_get_xdg_surface(XDGWMBase, es->w_surface);

	xdg_surface_add_listener(XDGSurface, &xdg_surface_listener, de);

	XDGToplevel = xdg_surface_get_toplevel(XDGSurface);
	xdg_toplevel_add_listener(XDGToplevel, &xdg_toplevel_listener, es);

	xdg_toplevel_set_title(XDGToplevel, "Wayland EGL example");
	if (fullscreen)
		xdg_toplevel_set_fullscreen(XDGToplevel, NULL);

	if (!es->x_decoration) {
		LOG("No decoration manager\n");
	}
	else {
		struct zxdg_toplevel_decoration_v1 * const decobj =
			zxdg_decoration_manager_v1_get_toplevel_decoration(es->x_decoration, XDGToplevel);
		zxdg_toplevel_decoration_v1_set_mode(decobj, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		zxdg_toplevel_decoration_v1_add_listener(decobj, &decoration_listener, es);
	}

#if W_SUBSURFACE
	es->w_surface2 = wl_compositor_create_surface(es->w_compositor);
	es->w_subsurface2 = wl_subcompositor_get_subsurface(es->w_subcompositor, es->w_surface2, es->w_surface);
	wl_subsurface_set_position(es->w_subsurface2, -20, -20);
	wl_subsurface_place_above(es->w_subsurface2, es->w_surface);
	wl_subsurface_set_sync(es->w_subsurface2);

	wl_surface_attach(es->w_surface2, draw_frame(es), 0, 0);
	wl_surface_commit(es->w_surface2);
#endif

	wl_surface_commit(es->w_surface);

	// This call the attached listener global_registry_handler
	wl_display_dispatch(es->native_display);
	wl_display_roundtrip(es->native_display);

	LOG("--- post round 2--\n");

// *****

//	while (wl_display_dispatch(es->native_display))
//	{
//  	/* This space deliberately left blank */
//	}

// *****

	if (is_egl)
		CreateWindowWithEGLContext(es, "Nya");
	else
		CreateWindowForDmaBuf(es, "Dma");

	LOG("<<< %s\n", __func__);

	for (i = 0; i != 32; ++i)
	{
		de->aux[i].fd = -1;
	}

	de->prod_fd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);

	assert(pthread_create(&de->q_thread, NULL, display_thread, de) == 0);

	sem_wait(&de->display_start_sem);

	if (de->q_terminate)
	{
		LOG("%s: Display startup failure\n", __func__);
		return NULL;
	}

	LOG(">>> %s\n", __func__);

	program_alive = true;

	return de;
}

struct egl_wayland_out_env* egl_wayland_out_new(bool fullscreen)
{
	return wayland_out_new(true, fullscreen);
}

struct egl_wayland_out_env* dmabuf_wayland_out_new(bool fullscreen)
{
	return wayland_out_new(false, fullscreen);
}

void egl_wayland_out_delete(struct egl_wayland_out_env *de)
{
	struct _escontext * const es = &ESContext;

	if (de == NULL)
		return;

	LOG("<<< %s\n", __func__);

	de->q_terminate = 1;
	display_prod(de);
	pthread_join(de->q_thread, NULL);
	if (de->prod_fd != -1)
		close(de->prod_fd);

	LOG(">>> %s\n", __func__);

	destroy_window(es);
	wl_display_disconnect(es->native_display);
	LOG("Display disconnected !\n");

	thing_q_uninit(&de->q_in);
	thing_q_uninit(&de->q_out);
	free(de);
}

static void
mkbar(struct dmabuf_h * const b, const unsigned int n2)
{
	uint8_t * buf;
	const unsigned int n = n2 & 127;
	unsigned int k;

	dmabuf_write_start(b);
	buf = dmabuf_map(b);

	for (k = 0; k < BUF_W; k += 128) {
		uint8_t * restrict p = buf + k * BUF_STRIDE2;
		unsigned int j;
		unsigned int i;

		j = 0;
		if (n > 128 - 16)
		{
			for (; j != n - (128 - 16); ++j)
				*p++ = BAR_Y;
		}
		for (; j != n; ++j)
			*p++ = BKG_Y;

		for (i = 0; i != BUF_H - 1; ++i) {
			for (j = 0; j != 16; ++j)
				*p++ = BAR_Y;
			for (j = 0; j != 128 - 16; ++j)
				*p++ = BKG_Y;
		}

		if (n > 128 - 16)
		{
			for (j = n; j != 128; ++j)
				*p++ = BAR_Y;
		}
		else
		{
			for (j = n; j != n + 16; ++j)
				*p++ = BAR_Y;
			for (; j != 128; ++j)
				*p++ = BKG_Y;
		}
	}

	dmabuf_write_end(b);
}

int
main(int argc, char *argv[])
{
	struct egl_wayland_out_env * const de = dmabuf_wayland_out_new(false);
	struct dmabufs_ctl * dbsc = dmabufs_ctl_new();
	unsigned int i;

	(void)argc;
	(void)argv;

	if (!de) {
		fprintf(stderr, "Failed to open window\n");
		return 1;
	}
	if (!dbsc) {
		fprintf(stderr, "Failed to open CMA\n");
		return 1;
	}

	thing_q_init(&de->q_in);
	thing_q_init(&de->q_out);

	for (i = 0; i != DBCOUNT; ++i) {
		struct dmabuf_h * buf;

		if ((buf = dmabuf_alloc(dbsc, BUFSIZE)) == NULL) {
			fprintf(stderr, "Failed to alloc buf %d, size %d\n", i, BUFSIZE);
			return 1;
		}
		dmabuf_write_start(buf);
		memset(dmabuf_map(buf), 0x80, BUFSIZE);
		dmabuf_write_end(buf);
		q_thing(&de->q_out, buf);
	}
	sem_init(&de->free_sem, 0, DBCOUNT);

	i = 0;
	for (;;) {
		struct dmabuf_h * buf;
		sem_wait(&de->free_sem);
		while ((buf = dq_thing(&de->q_out)) != NULL) {
			mkbar(buf, i++);
			q_thing(&de->q_in, buf);
		}
		printf("Q len: %d\n", (de->q_in.p_in - de->q_in.p_out) & (FQLEN - 1));
		display_prod(de);
	}

	dmabufs_ctl_unref(&dbsc);

	return 0;
}

