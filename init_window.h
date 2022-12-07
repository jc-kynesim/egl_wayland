#include "libavutil/frame.h"

struct egl_wayland_out_env;
typedef struct egl_wayland_out_env egl_wayland_out_env_t;

void egl_wayland_out_modeset(struct egl_wayland_out_env * dpo, int w, int h, AVRational frame_rate);
int egl_wayland_out_display(struct egl_wayland_out_env * dpo, AVFrame * frame);
struct egl_wayland_out_env * egl_wayland_out_new(void);
void egl_wayland_out_delete(struct egl_wayland_out_env * dpo);


