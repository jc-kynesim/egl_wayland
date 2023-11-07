// gcc -o test init_window.c -I. -lwayland-client -lwayland-server -lwayland-client-protocol -lwayland-egl -lEGL -lGLESv2
#include <wayland-client-core.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wayland-client-protocol.h>
#include <wayland-egl.h> // Wayland EGL MUST be included before EGL headers

#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#define LOG printf

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <libdrm/drm_fourcc.h>

#include <sys/time.h>

#include <pthread.h>

#include "picpool.h"
#include "dmabuf_alloc.h"

#define TRACE_ALL 1
#define  DEBUG_SOLID 0

#define PACE_TIME 12000

#define ES_SIG 0x12345678

struct wl_egl_window *egl_window;
struct wl_region *region;

struct xdg_wm_base *XDGWMBase;
struct xdg_surface *XDGSurface;
struct xdg_toplevel *XDGToplevel;

struct _escontext {
    struct wl_display *native_display;
    int window_width;
    int window_height;
    int req_w;
    int req_h;
    struct wl_egl_window *native_window;
    struct wl_compositor *w_compositor;

    struct wl_surface *w_surface;
    struct {
        struct wl_surface *surface;
        struct wl_subsurface *subsurface;
        struct wl_surface *surface2;
        struct wl_subsurface *subsurface2;
    } subs[8];

    struct zwp_linux_dmabuf_v1 *linux_dmabuf_v1_bind;
    struct wp_single_pixel_buffer_manager_v1 *single_pixel_buffer_manager_v1;
    struct wl_shm *w_shm;
    struct wl_subcompositor *w_subcompositor;
    struct zxdg_decoration_manager_v1 *x_decoration;
    struct wp_viewporter *w_viewporter;
    struct wp_viewport *w_viewport;
    unsigned int sig;
};

struct _escontext ESContext = {
    .native_display = NULL,
    .window_width = 0,
    .window_height = 0,
    .native_window  = 0,
    .sig = ES_SIG
};

struct egl_wayland_out_env {
    struct _escontext *es;

    int show_all;
    int window_width, window_height;
    int window_x, window_y;
    int fullscreen;
    bool use_shm;
    bool chequerboard;
    bool single_pixel;

    int offset_x;
    int offset_y;

    picpool_ctl_t * subpic_pool;

};

#define TRUE 1
#define FALSE 0

#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 720


static void xdg_toplevel_handle_configure(void *data,
                                          struct xdg_toplevel *xdg_toplevel, int32_t w, int32_t h,
                                          struct wl_array *states)
{
    struct _escontext *const es = data;
    (void)xdg_toplevel;
    uint32_t *p;

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
}

static void
xdg_toplevel_configure_bounds_cb(void *data,
                                 struct xdg_toplevel *xdg_toplevel,
                                 int32_t width,
                                 int32_t height)
{
    (void)data;
    LOG("%s[%p]: %dx%d\n", __func__, (void *)xdg_toplevel, width, height);
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
    LOG("%s[%p]:\n", __func__, (void *)xdg_toplevel);
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
    (void)data;
    LOG("%s\n", __func__);
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

void CreateNativeWindow(struct _escontext *const es, char *title)
{
    (void)title;

    region = wl_compositor_create_region(es->w_compositor);

    wl_region_add(region, 0, 0, es->req_w, es->req_h);
    wl_surface_set_opaque_region(es->w_surface, region);

    LOG("%s: %dx%d\n", __func__, es->req_w, es->req_h);
    es->window_width = es->req_w;
    es->window_height = es->req_h;
}

static void CreateWindowForDmaBuf(struct _escontext *const es, char *title)
{
    CreateNativeWindow(es, title);
}

static void linux_dmabuf_v1_listener_format(void *data,
                                            struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
                                            uint32_t format)
{
    // Superceeded by _modifier
    struct _escontext *const es = data;
    (void)zwp_linux_dmabuf_v1;
    (void)format;
    printf("%s[%p], %x\n", __func__, (void *)es, format);
}

static void
linux_dmabuf_v1_listener_modifier(void *data,
                                  struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
                                  uint32_t format,
                                  uint32_t modifier_hi,
                                  uint32_t modifier_lo)
{
    struct _escontext *const es = data;
    (void)zwp_linux_dmabuf_v1;

    printf("%s[%p], %x %08x%08x\n", __func__, (void *)es, format, modifier_hi, modifier_lo);
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
    printf("%s[%p]: mode %d\n", __func__, (void *)zxdg_toplevel_decoration_v1, mode);
    zxdg_toplevel_decoration_v1_destroy(zxdg_toplevel_decoration_v1);
}

static struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void global_registry_handler(void *data, struct wl_registry *registry, uint32_t id,
                                    const char *interface, uint32_t version)
{
    struct _escontext *const es = data;
    (void)version;

    LOG("Got a registry event for %s id %d\n", interface, id);
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        es->w_compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        es->linux_dmabuf_v1_bind = wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(es->linux_dmabuf_v1_bind, &linux_dmabuf_v1_listener, es);
    }
    if (strcmp(interface, wl_shm_interface.name) == 0)
        es->w_shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    if (strcmp(interface, wl_subcompositor_interface.name) == 0)
        es->w_subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        XDGWMBase = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(XDGWMBase, &xdg_wm_base_listener, NULL);
    }
    if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
        es->x_decoration = wl_registry_bind(registry, id, &zxdg_decoration_manager_v1_interface, 1);
    if (strcmp(interface, wp_viewporter_interface.name) == 0)
        es->w_viewporter = wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
    if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0)
        es->single_pixel_buffer_manager_v1 = wl_registry_bind(registry, id, &wp_single_pixel_buffer_manager_v1_interface, 1);
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
get_server_references(struct _escontext *const es)
{

    struct wl_display *display = wl_display_connect(NULL);
    if (display == NULL) {
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
    if (es->w_compositor == NULL || XDGWMBase == NULL) {
        LOG("No compositor !? No XDG !! There's NOTHING in here !\n");
        exit(1);
    }
    else {
        LOG("Okay, we got a compositor and a shell... That's something !\n");
        es->native_display = display;
    }
}

void destroy_window(struct _escontext *const es)
{
    wl_surface_destroy(es->w_surface);
}



static void
chequerboard(uint32_t *const data, unsigned int stride, const unsigned int width, const unsigned int height)
{
    stride /= sizeof(uint32_t);

    /* Draw checkerboxed background */
    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            if ((x + y / 8 * 8) % 16 < 8)
                data[y * stride + x] = 0xFF666666;
            else
                data[y * stride + x] = 0xFFEEEEEE;
        }
    }
}

static void
fill_uniform(uint32_t *const data, unsigned int stride, const unsigned int width, const unsigned int height, const uint32_t val)
{
    stride /= sizeof(uint32_t);

    /* Draw solid background */
    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x)
            data[y * stride + x] = val;
    }
}

static void
tl_half_arrow(uint32_t *const data, unsigned int stride_b, const unsigned int width, const unsigned int height)
{
    unsigned int stride = stride_b / sizeof(uint32_t);

    fill_uniform(data, stride_b, width, height, 0xff666666);
    for (unsigned int y = 0; y < height / 3; ++y) {
        unsigned int rhs = width * y / (height/2);
        for (unsigned int x = rhs; x < width*2/3 ; ++x) {
            data[y * stride + x] = 0xFFEEEEEE;
        }
    }
}

static const enum wl_output_transform transforms[8] = {
    WL_OUTPUT_TRANSFORM_NORMAL,
    WL_OUTPUT_TRANSFORM_FLIPPED,
    WL_OUTPUT_TRANSFORM_FLIPPED_180,
    WL_OUTPUT_TRANSFORM_180,
    WL_OUTPUT_TRANSFORM_FLIPPED_270,
    WL_OUTPUT_TRANSFORM_90,
    WL_OUTPUT_TRANSFORM_270,
    WL_OUTPUT_TRANSFORM_FLIPPED_90,
};

#define FILL_ARROW 2
#define FILL_CHEQUERBOARD 1
#define FILL_UNIFORM 0

static int
make_pic(struct egl_wayland_out_env *de, struct _escontext *const es, struct wl_surface * const surface,
         const unsigned int width, const unsigned int height, const int fillpat)
{
    // Build a background
    // This would be a perfect use of the single_pixel_surface extension
    // However we don't seem to support it
    struct dmabuf_h * dh = NULL;

    unsigned int stride = ((width + 15) & ~15)  * 4;
    struct wl_buffer * w_buffer;

    if ((dh = picpool_get(de->subpic_pool, stride * height)) == NULL) {
        LOG("Failed to get DmaBuf for background\n");
        goto error;
    }

    dmabuf_write_start(dh);
    switch (fillpat) {
    case FILL_ARROW:
        tl_half_arrow(dmabuf_map(dh), stride, width, height);
        break;
    case FILL_CHEQUERBOARD:
        chequerboard(dmabuf_map(dh), stride, width, height);
        break;
    default:
        fill_uniform(dmabuf_map(dh), stride, width, height, 0xff800000);
        break;
    }
    dmabuf_write_end(dh);

    if (de->use_shm)
    {
        struct wl_shm_pool *pool = wl_shm_create_pool(es->w_shm, dmabuf_fd(dh), dmabuf_size(dh));
        if (pool == NULL)
        {
            LOG("Failed to create pool from dmabuf\n");
            goto error;
        }
        w_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
    }
    else
    {
        struct zwp_linux_buffer_params_v1 *params;
        params = zwp_linux_dmabuf_v1_create_params(es->linux_dmabuf_v1_bind);
        if (!params) {
            LOG("zwp_linux_dmabuf_v1_create_params FAILED\n");
            goto error;
        }
        zwp_linux_buffer_params_v1_add(params, dmabuf_fd(dh), 0, 0, stride, 0, 0);
        w_buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, DRM_FORMAT_XRGB8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
    }
    if (!w_buffer) {
        LOG("Failed to create background buffer\n");
        goto error;
    }

//    wl_buffer_add_listener(w_buffer, &w_buffer_listener, dh);
    wl_surface_attach(surface, w_buffer, 0, 0);
    dh = NULL;

    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);
    return 0;

error:
    dmabuf_unref(&dh);
    return -1;
}

static void usage()
{
    LOG("Usage: fstest [s][f] <x> <y>\n"
        "  s Use dmabuf for buffers - otherwise shm\n"
        "  f Set fullscreen\n"
        );
    exit(1);
}

int main(int argc, char *argv[])
{
    struct egl_wayland_out_env *de = calloc(1, sizeof(*de));
    struct _escontext *const es = &ESContext;
    unsigned int i;
    unsigned int grid;
    const unsigned int gap_h = 16;
    const unsigned int gap_v = 32;

    de->es = es;
    de->use_shm = true;;

    es->req_w = WINDOW_WIDTH;
    es->req_h = WINDOW_HEIGHT;

    for (++argv; argc > 1; argc--, argv++) {
        const char * p = *argv;
        char c;
        while ((c = *p++) != 0) {
            switch (c) {
            case 'd':
                de->use_shm = false;
                break;
            case 'f':
                de->fullscreen = true;
                break;
            default:
                usage();
            }
        }
    }

    get_server_references(es);

    es->w_surface = wl_compositor_create_surface(es->w_compositor);
    if (es->w_surface == NULL) {
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
    if (de->fullscreen)
        xdg_toplevel_set_fullscreen(XDGToplevel, NULL);

    if (!es->x_decoration) {
        LOG("No decoration manager\n");
    }
    else {
        struct zxdg_toplevel_decoration_v1 *const decobj =
            zxdg_decoration_manager_v1_get_toplevel_decoration(es->x_decoration, XDGToplevel);
        zxdg_toplevel_decoration_v1_set_mode(decobj, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        zxdg_toplevel_decoration_v1_add_listener(decobj, &decoration_listener, es);
    }

    {
        unsigned int grid_a = (WINDOW_WIDTH - gap_h * 5) / 4;
        unsigned int grid_b = (WINDOW_HEIGHT - gap_v * 3) / 2;
        grid = grid_a < grid_b ? grid_a : grid_b;

        for (i = 0; i != 8; i++) {
            struct wl_surface * const surface = wl_compositor_create_surface(es->w_compositor);
            struct wl_subsurface * const subsurface = wl_subcompositor_get_subsurface(es->w_subcompositor, surface, es->w_surface);
            es->subs[i].surface = surface;
            es->subs[i].subsurface = subsurface;
            wl_surface_set_buffer_transform(surface, transforms[i]);
            wl_subsurface_place_above(subsurface, es->w_surface);
            wl_subsurface_set_position(subsurface, gap_h + (grid + gap_h) * (i % 4), gap_v + (grid + gap_v) * (i / 4));
        }

        for (i = 0; i != 8; i++) {
            struct wl_surface * const parent = es->subs[i].surface;
            struct wl_surface * const surface = wl_compositor_create_surface(es->w_compositor);
            struct wl_subsurface * const subsurface = wl_subcompositor_get_subsurface(es->w_subcompositor, surface, parent);
            es->subs[i].surface2 = surface;
            es->subs[i].subsurface2 = subsurface;
            wl_subsurface_place_above(subsurface, parent);
            wl_subsurface_set_position(subsurface, gap_h, gap_v);
            wl_surface_commit(surface);
            wl_surface_commit(parent);
        }
    }

    wl_surface_commit(es->w_surface);

    // This call the attached listener global_registry_handler
    wl_display_roundtrip(es->native_display);

    LOG("--- post round 2--\n");

    {
        struct dmabufs_ctl *dbsc = de->use_shm ? dmabufs_shm_new() : dmabufs_ctl_new();
        if (dbsc == NULL)
        {
            LOG("Failed to create dmabuf ctl");
            goto error;
        }
        de->subpic_pool = picpool_new(dbsc);
        dmabufs_ctl_unref(&dbsc);
        if (de->subpic_pool == NULL)
        {
            LOG("Failed to create picpool");
            goto error;
        }
    }

    CreateWindowForDmaBuf(es, "Dma");

    for (i = 0; i != 8; i++) {
        make_pic(de, es, es->subs[i].surface2, gap_h, gap_v, FILL_UNIFORM);
        make_pic(de, es, es->subs[i].surface, grid, grid * 3/ 4, FILL_ARROW);
    }

    make_pic(de, es, es->w_surface, WINDOW_WIDTH, WINDOW_HEIGHT, FILL_CHEQUERBOARD);

    wl_display_roundtrip(es->native_display);

    sleep(20);

    LOG(">>> %s\n", __func__);

    destroy_window(es);
    wl_display_disconnect(es->native_display);
    LOG("Display disconnected !\n");

    free(de);

    return 0;

error:
    LOG("Error exit\n");
    return 1;
}

