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

#include <libdrm/drm_fourcc.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "pollqueue.h"

#define ES_SIG 0x12345678

typedef struct eq_env_ss {
    atomic_int eq_count;
    sem_t sem;
    bool rsync;

    struct wl_display *display;
    struct pollqueue *pq;
    struct wl_event_queue *q;
    struct wl_display *wrapped_display;
} eq_env_t;


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

	int seen_fmt;

    eq_env_t *eq;
    struct pollqueue *pq;

	unsigned int sig;
};

struct _escontext ESContext = {
	.native_display = NULL,
	.window_width = 0,
	.window_height = 0,
	.native_window  = 0,
	.sig = ES_SIG
};

#define LOG printf


// ----------------------------------------------------------------------------

static struct wl_display *
eq_wrapper(eq_env_t * const eq)
{
    return eq->wrapped_display;
}

#if 0
static void
eq_ref(eq_env_t * const eq)
{
    int n;
    n = atomic_fetch_add(&eq->eq_count, 1);
    fprintf(stderr, "Ref: count=%d\n", n + 1);
}
#endif

static void
eq_unref(eq_env_t ** const ppeq)
{
    int n;
    eq_env_t * eq = *ppeq;
    if (eq != NULL)
    {
        *ppeq = NULL;
        n = atomic_fetch_sub(&eq->eq_count, 1);
        fprintf(stderr, "Unref: Buffer count=%d\n", n);
        if (n == 0)
            sem_post(&eq->sem);
    }
}

static int
eq_finish(eq_env_t ** const ppeq)
{
    eq_env_t * const eq = *ppeq;
    int rv;
    struct timespec ts;

    if (eq == NULL)
        return 0;

    eq_unref(ppeq);

    if ((rv = clock_gettime(CLOCK_REALTIME, &ts)) != -1)
    {
        ts.tv_sec += 1;
        while ((rv = sem_timedwait(&eq->sem, &ts)) != 0 && errno == EINTR)
            /* Loop */;
    }

    // Leak rather than crash if we actually get the missing buffers returned
    if (rv != 0)
        return rv;

    wl_proxy_wrapper_destroy(eq->wrapped_display);
    wl_event_queue_destroy(eq->q);

    pollqueue_set_pre_post(eq->pq, 0, 0, NULL);
    pollqueue_unref(&eq->pq);
    sem_destroy(&eq->sem);
    free(eq);
    fprintf(stderr, "Eq closed\n");
    return 0;
}

static void
pollq_pre_cb(void * v, struct pollfd * pfd)
{
    eq_env_t * const eq = v;
    struct wl_display *const display = eq->display;
    int ferr;
    int frv;

    fprintf(stderr, "Start Prepare\n");

    while (wl_display_prepare_read_queue(display, eq->q) != 0) {
        int n = wl_display_dispatch_queue_pending(display, eq->q);
        fprintf(stderr, "Dispatch=%d\n", n);
    }
    if ((frv = wl_display_flush(display)) >= 0) {
        pfd->events = POLLIN;
        ferr = 0;
    }
    else {
        ferr = errno;
        pfd->events = POLLOUT | POLLIN;
    }
    pfd->fd = wl_display_get_fd(display);

    fprintf(stderr, "Done Prepare: fd=%d, evts=%#x, frv=%d, ferr=%s\n", pfd->fd, pfd->events, frv, ferr == 0 ? "ok" : strerror(ferr));
}

static void
pollq_post_cb(void *v, short revents)
{
    eq_env_t * const eq = v;
    struct wl_display *const display = eq->display;

    if ((revents & POLLIN) == 0) {
        fprintf(stderr, "Cancel read: Events=%#x: IN=%#x, OUT=%#x, ERR=%#x\n", (int)revents, POLLIN, POLLOUT, POLLERR);
        wl_display_cancel_read(display);
    }
    else {
        fprintf(stderr, "Read events: Events=%#x: IN=%#x, OUT=%#x, ERR=%#x\n", (int)revents, POLLIN, POLLOUT, POLLERR);
        wl_display_read_events(display);
    }

    fprintf(stderr, "Start Dispatch\n");
    int n =
    wl_display_dispatch_queue_pending(display, eq->q);
    fprintf(stderr, "Dispatch=%d\n", n);
}

static eq_env_t *
eq_new(struct wl_display * const display, struct pollqueue * const pq)
{
    eq_env_t * eq = calloc(1, sizeof(*eq));

    if (eq == NULL)
        return NULL;

    atomic_init(&eq->eq_count, 0);
    sem_init(&eq->sem, 0, 0);

    if ((eq->q = wl_display_create_queue(display)) == NULL)
        goto err1;
    if ((eq->wrapped_display = wl_proxy_create_wrapper(display)) == NULL)
        goto err2;
    wl_proxy_set_queue((struct wl_proxy *)eq->wrapped_display, eq->q);

    eq->display = display;
    eq->pq = pollqueue_ref(pq);

    pollqueue_set_pre_post(eq->pq, pollq_pre_cb, pollq_post_cb, eq);
    return eq;

err2:
    wl_event_queue_destroy(eq->q);
err1:
    free(eq);
    return NULL;
}

static void eventq_sync_cb(void * data, struct wl_callback * cb, unsigned int cb_data)
{
    sem_t * const sem = data;
    (void)(cb_data);
    wl_callback_destroy(cb);
    sem_post(sem);
    fprintf(stderr, "Sync cb: Q %p\n", ((void**)cb)[4]);
}

static const struct wl_callback_listener eq_sync_listener = {.done = eventq_sync_cb};

// The rsync fns are a kludge
// For reasons unknown sometimes the add_listener / sync sequence doesn't
// give the sync after the results (shm in particular). This allows us to wait
// for the results to start appearing before asking for a callback.

static void
eventq_rsync_post(eq_env_t * const eq)
{
    struct wl_callback * cb;

    // Arguably atomic would be good here but we don't expect this to be
    // called from from than one thread
    if (!eq->rsync)
        return;
    eq->rsync = false;

    cb = wl_display_sync(eq_wrapper(eq));
    wl_callback_add_listener(cb, &eq_sync_listener, &eq->sem);
    wl_display_flush(eq->display);
}

static int
eventq_rsync_wait(eq_env_t * const eq)
{
    int rv;

    if (!eq)
        return -1;

    wl_display_flush(eq->display);
    while ((rv = sem_wait(&eq->sem)) == -1 && errno == EINTR)
        /* Loop */;

    return rv;
}

static int
eventq_rsync_set(eq_env_t * const eq)
{
    if (!eq || eq->rsync)
        return -1;
    eq->rsync = true;
    return 0;
}

#if 0
static int
eventq_sync(struct _escontext *const es)
{
    sem_t sem;
    struct wl_callback * cb;
    int rv;

    sem_init(&sem, 0, 0);
    cb = wl_display_sync(eq_wrapper(es->eq));
    wl_callback_add_listener(cb, &listener, &sem);
    wl_display_flush(es->native_display);
    while ((rv = sem_wait(&sem)) == -1 && errno == EINTR)
        /* Loop */;
    sem_destroy(&sem);
    return rv;
}
#endif

static void global_registry_handler(void *data, struct wl_registry *registry, uint32_t id,
									const char *interface, uint32_t version)
{
	struct _escontext * const es = data;
	(void)version;

    eventq_rsync_post(es->eq);

//	LOG("Got a registry event for %s id %d\n", interface, id);
	if (strcmp(interface, wl_compositor_interface.name) == 0)
		es->w_compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
	if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
		es->linux_dmabuf_v1_bind = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 3);
	if (strcmp(interface, wl_shm_interface.name) == 0)
		es->w_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
	if (strcmp(interface, wl_subcompositor_interface.name) == 0)
		es->w_subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
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
	LOG("Got a display !\n");

	es->native_display = display;
    es->eq = eq_new(display, es->pq);

	struct wl_registry *wl_registry =
		wl_display_get_registry(eq_wrapper(es->eq));
    eventq_rsync_set(es->eq);
	wl_registry_add_listener(wl_registry, &listener, &ESContext);

	// This call the attached listener global_registry_handler
    eventq_rsync_wait(es->eq);

	// If at this point, global_registry_handler didn't set the
	// compositor, nor the shell, bailout !
	if (es->w_compositor == NULL)
	{
		LOG("No compositor !? No XDG !! There's NOTHING in here !\n");
		exit(1);
	}
}

static void shm_listener_format(void *data,
               struct wl_shm *shm,
               uint32_t format)
{
    struct _escontext * const es = data;
    (void)shm;

    eventq_rsync_post(es->eq);

    if (format == 0)
        format = DRM_FORMAT_ARGB8888;
    else if (format == 1)
        format = DRM_FORMAT_XRGB8888;

    LOG("%s[%p], %.4s: Q %p\n", __func__, (void*)es, (const char *)&format, ((void**)shm)[4]);

	es->seen_fmt = 1;
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_listener_format,
};

static void linux_dmabuf_v1_listener_format(void *data,
               struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
               uint32_t format)
{
    // Superceeded by _modifier
    (void)data;
    (void)zwp_linux_dmabuf_v1;

    LOG("%s[%p], %.4s", __func__, data, (const char *)&format);
}

static void
linux_dmabuf_v1_listener_modifier(void *data,
         struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
         uint32_t format,
         uint32_t modifier_hi,
         uint32_t modifier_lo)
{
    (void)zwp_linux_dmabuf_v1;
    (void)data;

    LOG("%s[%p], %.4s %08x%08x\n", __func__, data, (const char *)&format, modifier_hi, modifier_lo);
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_v1_listener = {
    .format = linux_dmabuf_v1_listener_format,
    .modifier = linux_dmabuf_v1_listener_modifier,
};


int
main(int argc, char * argv[])
{
	struct _escontext * const es = &ESContext;
	(void)argc;
	(void)argv;

    es->pq = pollqueue_new();

	get_server_references(es);

    zwp_linux_dmabuf_v1_add_listener(es->linux_dmabuf_v1_bind, &linux_dmabuf_v1_listener, NULL);

    eventq_rsync_set(es->eq);
    wl_shm_add_listener(es->w_shm, &shm_listener, es);
    eventq_rsync_wait(es->eq);

	if (es->seen_fmt)
		LOG("** Seen format\n");
	else
		LOG("**** Format missing ****");

    eq_finish(&es->eq);
}

