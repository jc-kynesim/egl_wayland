// gcc -o test init_window.c -I. -lwayland-client -lwayland-server -lwayland-client-protocol -lwayland-egl -lEGL -lGLESv2
#include <wayland-client-core.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h> // Wayland EGL MUST be included before EGL headers

#include "xdg-shell-client-protocol.h"

#include "init_window.h"
//#include "log.h"
#define LOG printf

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <math.h>

#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/time.h>

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include <pthread.h>
#include <semaphore.h>

#include "libavutil/frame.h"
#include "libavcodec/avcodec.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"

struct wl_compositor *compositor = NULL;
struct wl_surface *surface;
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
	struct wl_egl_window *native_window;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
};

struct _escontext ESContext = {
	.native_display = NULL,
	.window_width = 0,
	.window_height = 0,
	.native_window  = 0,
	.display = NULL,
	.context = NULL,
	.surface = NULL
};

typedef struct egl_aux_s
{
	int fd;
	GLuint texture;
} egl_aux_t;

struct egl_wayland_out_env
{
	enum AVPixelFormat avfmt;

	int show_all;
	int window_width, window_height;
	int window_x, window_y;
	int fullscreen;

	egl_aux_t aux[32];

	pthread_t q_thread;
	pthread_mutex_t q_lock;
	sem_t display_start_sem;
	sem_t q_sem;
	int prod_fd;
	int q_terminate;
	AVFrame *q_this;
	AVFrame *q_next;
};

#define TRUE 1
#define FALSE 0

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720

bool program_alive;
int32_t old_w, old_h;


static int do_display(egl_wayland_out_env_t *const de, const struct _escontext *const es, AVFrame *const frame)
{
	const AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
	egl_aux_t *da = NULL;
	unsigned int i;

#if TRACE_ALL
	LOG("<<< %s\n", __func__);
#endif

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

	// Make the context current
	if (!eglMakeCurrent(es->display, es->surface, es->surface, es->context))
	{
		LOG("Could not make the current window current !\n");
		goto fail;
	}

	if (!epoxy_has_egl_extension(es->display, "EGL_KHR_image_base"))
	{
		LOG("Missing EGL KHR image extension\n");
		goto fail;
	}

	if (gl_setup())
	{
		LOG("%s: gl_setup failed\n", __func__);
		goto fail;
	}

#if TRACE_ALL
	LOG("--- %s: Start done\n", __func__);
#endif
	sem_post(&de->display_start_sem);

	wl_fd = wl_display_get_fd(es->native_display);

	while (!de->q_terminate)
	{
		AVFrame *frame;
		int rv;

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

		do {
			rv = poll(pollfds, 2, -1);
		} while (rv < 0 && errno == EINTR);

		if (rv < 0)
		{
			LOG("Poll failed: %s\n", strerror(errno));
			break;
		}
		if (rv == 0)
		{
			LOG("Poll unexpected timeout\n");
			break;
		}

		if (wl_display_read_events(es->native_display) != 0)
			LOG("Read Event Failed\n");

		if (pollfds[0].revents)
		{
			uint64_t rcount = 0;
			if (read(de->prod_fd, &rcount, sizeof(rcount)) != sizeof(rcount))
				LOG("Unrexpected prod read");

			pthread_mutex_lock(&de->q_lock);
			frame = de->q_next;
			de->q_next = NULL;
			pthread_mutex_unlock(&de->q_lock);

			if (frame)
			{
				do_display(de, es, frame);
				av_frame_free(&de->q_this);
				de->q_this = frame;
			}
		}
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
	(void)data;
	(void)xdg_toplevel;
	(void)states;

	// no window geometry event, ignore
	if (w == 0 && h == 0)
		return;

	// window resized
	if (old_w != w && old_h != h)
	{
		old_w = w;
		old_h = h;

		wl_egl_window_resize(ESContext.native_window, w, h, 0, 0);
		wl_surface_commit(surface);
	}
}

static void xdg_toplevel_handle_close(void *data,
									  struct xdg_toplevel *xdg_toplevel)
{
	(void)data;
	(void)xdg_toplevel;

	// window closed, be sure that this event gets processed
	program_alive = false;
}

struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
};


static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
								  uint32_t serial)
{
	(void)data;
	// confirm that you exist to the compositor
	xdg_surface_ack_configure(xdg_surface, serial);

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

void CreateNativeWindow(char *title, int width, int height)
{
	(void)title;

	old_w = WINDOW_WIDTH;
	old_h = WINDOW_HEIGHT;

	region = wl_compositor_create_region(compositor);

	wl_region_add(region, 0, 0, width, height);
	wl_surface_set_opaque_region(surface, region);

	struct wl_egl_window *egl_window =
		wl_egl_window_create(surface, width, height);

	if (egl_window == EGL_NO_SURFACE)
	{
		LOG("No window !?\n");
		exit(1);
	}
	else
		LOG("Window created !\n");
	ESContext.window_width = width;
	ESContext.window_height = height;
	ESContext.native_window = egl_window;

}

EGLBoolean CreateEGLContext()
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
		EGL_RED_SIZE,        8,
		EGL_GREEN_SIZE,      8,
		EGL_BLUE_SIZE,       8,
		EGL_NONE
	};
	EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE, EGL_NONE };
	EGLDisplay display = eglGetDisplay(ESContext.native_display);
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

	// Get configs
	if ((eglGetConfigs(display, NULL, 0, &numConfigs) != EGL_TRUE) || (numConfigs == 0))
	{
		LOG("No configuration...\n");
		return EGL_FALSE;
	}

	// Choose config
	if ((eglChooseConfig(display, fbAttribs, &config, 1, &numConfigs) != EGL_TRUE) || (numConfigs != 1))
	{
		LOG("No configuration...\n");
		return EGL_FALSE;
	}

	// Create a surface
	surface = eglCreateWindowSurface(display, config, ESContext.native_window, NULL);
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

	ESContext.display = display;
	ESContext.surface = surface;
	ESContext.context = context;
	return EGL_TRUE;
}

EGLBoolean CreateWindowWithEGLContext(char *title, int width, int height)
{
	CreateNativeWindow(title, width, height);
	return CreateEGLContext();
}

void draw()
{
	static double a = 0.3;

	glClearColor(0.5, a, 0.0, 1.0);

	a += 0.05;
	if (a >= 1.0)
		a = 0.0;

	//struct timeval tv;

	//gettimeofday(&tv, NULL);

	//float time = tv.tv_sec + tv.tv_usec/1000000.0;

	//static GLfloat vertex_data[] = {
	//	0.6, 0.6, 1.0,
	//	-0.6, -0.6, 1.0,
	//	0.0, 1.0, 1.0
	//};

	//for(int i=0; i<3; i++) {
	//	vertex_data[i*3+0] = vertex_data[i*3+0]*cos(time) - vertex_data[i*3+1]*sin(time);
	//	vertex_data[i*3+1] = vertex_data[i*3+0]*sin(time) + vertex_data[i*3+1]*cos(time);
	//}

	glClear(GL_COLOR_BUFFER_BIT);

	//glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, vertex_data);
	//glEnableVertexAttribArray(0);

	//glDrawArrays(GL_TRIANGLES, 0, 3);
}

unsigned long last_click = 0;
void RefreshWindow()
{
	eglSwapBuffers(ESContext.display, ESContext.surface);
}

static void global_registry_handler(void *data, struct wl_registry *registry, uint32_t id,
									const char *interface, uint32_t version)
{
	(void)data;
	(void)version;

	LOG("Got a registry event for %s id %d\n", interface, id);
	if (strcmp(interface, "wl_compositor") == 0)
		compositor =
			wl_registry_bind(registry, id, &wl_compositor_interface, 1);
	else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
	{
		XDGWMBase = wl_registry_bind(registry, id,
									 &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(XDGWMBase, &xdg_wm_base_listener, NULL);
	}
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
get_server_references()
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
	wl_registry_add_listener(wl_registry, &listener, NULL);

	// This call the attached listener global_registry_handler
	wl_display_dispatch(display);
	wl_display_roundtrip(display);

	// If at this point, global_registry_handler didn't set the
	// compositor, nor the shell, bailout !
	if (compositor == NULL || XDGWMBase == NULL)
	{
		LOG("No compositor !? No XDG !! There's NOTHING in here !\n");
		exit(1);
	}
	else
	{
		LOG("Okay, we got a compositor and a shell... That's something !\n");
		ESContext.native_display = display;
	}
}

void destroy_window()
{
	eglDestroySurface(ESContext.display, ESContext.surface);
	wl_egl_window_destroy(ESContext.native_window);
	xdg_toplevel_destroy(XDGToplevel);
	xdg_surface_destroy(XDGSurface);
	wl_surface_destroy(surface);
	eglDestroyContext(ESContext.display, ESContext.context);
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

int egl_wayland_out_display(struct egl_wayland_out_env *de, AVFrame *src_frame)
{
	AVFrame *frame = NULL;

#if TRACE_ALL
	LOG("%s\n", __func__);
#endif

	if (src_frame->format == AV_PIX_FMT_DRM_PRIME)
	{
		frame = av_frame_alloc();
		av_frame_ref(frame, src_frame);
	}
	else if (src_frame->format == AV_PIX_FMT_VAAPI)
	{
		frame = av_frame_alloc();
		frame->format = AV_PIX_FMT_DRM_PRIME;
		if (av_hwframe_map(frame, src_frame, 0) != 0)
		{
			LOG("Failed to map frame (format=%d) to DRM_PRiME\n", src_frame->format);
			av_frame_free(&frame);
			return AVERROR(EINVAL);
		}
	}
	else
	{
		LOG("Frame (format=%d) not DRM_PRiME\n", src_frame->format);
		return AVERROR(EINVAL);
	}

	// Really hacky sync
	while (de->show_all && de->q_next)
	{
		usleep(3000);
	}

	pthread_mutex_lock(&de->q_lock);
	{
		AVFrame *const t = de->q_next;
		de->q_next = frame;
		frame = t;
	}
	pthread_mutex_unlock(&de->q_lock);

	if (frame == NULL)
		display_prod(de);
	else
		av_frame_free(&frame);

	return 0;
}

struct egl_wayland_out_env* egl_wayland_out_new(void)
{
	struct egl_wayland_out_env *de = malloc(sizeof(*de));
	unsigned int i;

	de->prod_fd = -1;
	de->q_terminate = 0;
	pthread_mutex_init(&de->q_lock, NULL);
	sem_init(&de->display_start_sem, 0, 0);

	get_server_references();

	surface = wl_compositor_create_surface(compositor);
	if (surface == NULL)
	{
		LOG("No Compositor surface ! Yay....\n");
		exit(1);
	}
	else
		LOG("Got a compositor surface !\n");

	XDGSurface = xdg_wm_base_get_xdg_surface(XDGWMBase, surface);

	xdg_surface_add_listener(XDGSurface, &xdg_surface_listener, NULL);

	XDGToplevel = xdg_surface_get_toplevel(XDGSurface);
	xdg_toplevel_set_title(XDGToplevel, "Wayland EGL example");
	xdg_toplevel_add_listener(XDGToplevel, &xdg_toplevel_listener, NULL);

	wl_surface_commit(surface);

	CreateWindowWithEGLContext("Nya", 1280, 720);

	LOG("<<< %s\n", __func__);

	for (i = 0; i != 32; ++i)
	{
		de->aux[i].fd = -1;
	}

    de->prod_fd = eventfd(0, EFD_NONBLOCK);

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

void egl_wayland_out_delete(struct egl_wayland_out_env *de)
{
	if (de == NULL)
		return;

	LOG("<<< %s\n", __func__);

	de->q_terminate = 1;
	display_prod(de);
	pthread_join(de->q_thread, NULL);
    if (de->prod_fd != -1)
        close(de->prod_fd);
    pthread_mutex_destroy(&de->q_lock);

	av_frame_free(&de->q_next);
	av_frame_free(&de->q_this);

	LOG(">>> %s\n", __func__);

	destroy_window();
	wl_display_disconnect(ESContext.native_display);
	LOG("Display disconnected !\n");

	free(de);
}

