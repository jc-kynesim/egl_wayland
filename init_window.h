#include "libavutil/frame.h"

struct egl_wayland_out_env;
typedef struct egl_wayland_out_env egl_wayland_out_env_t;

#define WOUT_FLAG_FULLSCREEN 1
#define WOUT_FLAG_NO_WAIT    2

void egl_wayland_out_modeset(struct egl_wayland_out_env * dpo, int w, int h, AVRational frame_rate);
int egl_wayland_out_display(struct egl_wayland_out_env * dpo, AVFrame * frame);
struct egl_wayland_out_env * egl_wayland_out_new(unsigned int flags);
struct egl_wayland_out_env * dmabuf_wayland_out_new(unsigned int flags);
void egl_wayland_out_delete(struct egl_wayland_out_env * dpo);


