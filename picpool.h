#ifndef _WAYLAND_PICPOOL_H
#define _WAYLAND_PICPOOL_H

#ifdef __cplusplus
extern "C" {
#endif

struct dmabuf_h;
struct dmabufs_ctl;
struct picpool_ctl_s;
typedef struct picpool_ctl_s picpool_ctl_t;

struct dmabuf_h * picpool_get(picpool_ctl_t * const pc, size_t req_size);

void picpool_flush(picpool_ctl_t * const pc);
void picpool_unref(picpool_ctl_t ** const pppc);
picpool_ctl_t * picpool_ref(picpool_ctl_t * const pc);
picpool_ctl_t * picpool_new(struct dmabufs_ctl * dbsc);

#ifdef __cplusplus
}
#endif

#endif
