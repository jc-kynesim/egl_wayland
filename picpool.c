#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>

#include <pthread.h>

#include "picpool.h"
#include "dmabuf_alloc.h"

// ===========================================================================

typedef struct pool_ent_s
{
    struct pool_ent_s * next;
    struct pool_ent_s * prev;

    atomic_int ref_count;
    unsigned int seq;

    struct picpool_ctl_s * pc;
    struct dmabuf_h * db;

    size_t size;
    unsigned int width;
    unsigned int height;
} pool_ent_t;


typedef struct ent_list_hdr_s
{
    pool_ent_t * ents;
    pool_ent_t * tail;
    unsigned int n;
} ent_list_hdr_t;

#define ENT_LIST_HDR_INIT (ent_list_hdr_t){ \
   .ents = NULL, \
   .tail = NULL, \
   .n = 0 \
}

struct picpool_ctl_s
{
    atomic_int ref_count;

    ent_list_hdr_t ent_pool;
    ent_list_hdr_t ents_cur;
    ent_list_hdr_t ents_prev;

    unsigned int max_n;
    unsigned int seq;

    pthread_mutex_t lock;
    struct dmabufs_ctl * dbsc;
};


static pool_ent_t * ent_extract(ent_list_hdr_t * const elh, pool_ent_t * const ent)
{
//    printf("List %p [%d]: Ext %p\n", elh, elh->n, ent);

    if (ent == NULL)
        return NULL;

    if (ent->next == NULL)
        elh->tail = ent->prev;
    else
        ent->next->prev = ent->prev;

    if (ent->prev == NULL)
        elh->ents = ent->next;
    else
        ent->prev->next = ent->next;

    ent->prev = ent->next = NULL;

    --elh->n;

    return ent;  // For convienience
}

static inline pool_ent_t * ent_extract_tail(ent_list_hdr_t * const elh)
{
    return ent_extract(elh, elh->tail);
}

static void ent_add_head(ent_list_hdr_t * const elh, pool_ent_t * const ent)
{
//    printf("List %p [%d]: Add %p\n", elh, elh->n, ent);

    if ((ent->next = elh->ents) == NULL)
        elh->tail = ent;
    else
        ent->next->prev = ent;

    ent->prev = NULL;
    elh->ents = ent;
    ++elh->n;
}

static void ent_free(pool_ent_t * const ent)
{
//    printf("Free ent: %p\n", ent);
    if (ent != NULL)
    {
        // If we still have a ref to a buffer - kill it now
        dmabuf_free(ent->db);
        free(ent);
    }
}

static void ent_free_list(ent_list_hdr_t * const elh)
{
    pool_ent_t * ent = elh->ents;

//    printf("Free list: %p [%d]\n", elh, elh->n);

    *elh = ENT_LIST_HDR_INIT;

    while (ent != NULL) {
        pool_ent_t * const t = ent;
        ent = t->next;
        ent_free(t);
    }
}

#if 0
static void ent_list_move(ent_list_hdr_t * const dst, ent_list_hdr_t * const src)
{
//    printf("Move %p->%p\n", src, dst);

    *dst = *src;
    *src = ENT_LIST_HDR_INIT;
}
#endif

#if 0
// Scans "backwards" as that should give us the fastest match if we are
// presented with pics in the same order each time
static pool_ent_t * ent_list_extract_pic_ent(ent_list_hdr_t * const elh, picture_t * const pic)
{
    pool_ent_t *ent = elh->tail;

//    printf("Find list: %p [%d]; pic:%p\n", elh, elh->n, pic);

    while (ent != NULL) {
//        printf("Check ent: %p, pic:%p\n", ent, ent->pic);

        if (ent->pic == pic)
            return ent_extract(elh, ent);
        ent = ent->prev;
    }
    return NULL;
}
#endif

#define POOL_ENT_ALLOC_BLOCK  0x10000

static pool_ent_t * pool_ent_alloc_new(picpool_ctl_t * const pc, size_t req_size)
{
    pool_ent_t * ent = calloc(1, sizeof(*ent));
    const size_t alloc_size = (req_size + POOL_ENT_ALLOC_BLOCK - 1) & ~(POOL_ENT_ALLOC_BLOCK - 1);

    if (ent == NULL)
        return NULL;

    ent->next = ent->prev = NULL;

    // Alloc
    if ((ent->db = dmabuf_realloc(pc->dbsc, NULL, alloc_size)) == NULL)
        goto fail1;
//    fprintf(stderr, "%s: ent %p db %p req=%zd size=%zd\n", __func__, ent, ent->db, req_size, alloc_size);
    ent->size = dmabuf_size(ent->db);
    return ent;

fail1:
    free(ent);
    return NULL;
}

static inline pool_ent_t * pool_ent_ref(pool_ent_t * const ent)
{
//    int n = atomic_fetch_add(&ent->ref_count, 1) + 1;
//    printf("Ref: %p: %d\n", ent, n);
    atomic_fetch_add(&ent->ref_count, 1);
    return ent;
}

static void pool_recycle(picpool_ctl_t * const pc, pool_ent_t * const ent)
{
    pool_ent_t * xs = NULL;
    int n;

    if (ent == NULL)
        return;

    n = atomic_fetch_sub(&ent->ref_count, 1) - 1;

//    fprintf(stderr, "%s: Pool: %p: Ent: %p: %d dh: %p\n", __func__, &pc->ent_pool, ent, n, ent->db);

    if (n != 0)
        return;

    pthread_mutex_lock(&pc->lock);

    // If we have a full pool then extract the LRU and free it
    // Free done outside mutex
    if (pc->ent_pool.n >= pc->max_n)
        xs = ent_extract_tail(&pc->ent_pool);

    ent_add_head(&pc->ent_pool, ent);

    pthread_mutex_unlock(&pc->lock);

    ent_free(xs);
}

// * This could be made more efficient, but this is easy
static void pool_recycle_list(picpool_ctl_t * const pc, ent_list_hdr_t * const elh)
{
    pool_ent_t * ent;
    while ((ent = ent_extract_tail(elh)) != NULL) {
        pool_recycle(pc, ent);
    }
}

static int pool_predel_cb(struct dmabuf_h * dh, void * v)
{
    pool_ent_t * const ent = v;
    picpool_ctl_t * pc = ent->pc;

    assert(ent->db == dh);

    ent->pc = NULL;
    dmabuf_ref(dh);
    dmabuf_predel_cb_unset(dh);
    pool_recycle(pc, ent);
    picpool_unref(&pc);
    return 1;  // Do not delete
}

struct dmabuf_h * picpool_get(picpool_ctl_t * const pc, size_t req_size)
{
    pool_ent_t * best = NULL;

    pthread_mutex_lock(&pc->lock);

    {
        pool_ent_t * ent = pc->ent_pool.ents;

        // Simple scan
        while (ent != NULL) {
            if (ent->size >= req_size && ent->size <= req_size * 2 + POOL_ENT_ALLOC_BLOCK &&
                    (best == NULL || best->size > ent->size))
                best = ent;
            ent = ent->next;
        }

        // extract best from chain if we've found it
        ent_extract(&pc->ent_pool, best);
    }

    pthread_mutex_unlock(&pc->lock);

    if (best == NULL) {
        if ((best = pool_ent_alloc_new(pc, req_size)) == NULL)
            return NULL;
    }

    if ((best->seq = ++pc->seq) == 0)
        best->seq = ++pc->seq;  // Never allow to be zero

    atomic_store(&best->ref_count, 1);
    best->pc = picpool_ref(pc);
    dmabuf_predel_cb_set(best->db, pool_predel_cb, best);
//    fprintf(stderr, "%s: find ent %p db %p size %zd\n", __func__, best, best->db, best->size);
    return best->db;
}

void picpool_flush(picpool_ctl_t * const pc)
{
    pool_recycle_list(pc, &pc->ents_prev);
    pool_recycle_list(pc, &pc->ents_cur);
}

static void picpool_delete(picpool_ctl_t * const pc)
{

//    printf("<<< %s\n", __func__);

    picpool_flush(pc);

    ent_free_list(&pc->ent_pool);

    dmabufs_ctl_unref(&pc->dbsc);

    pthread_mutex_destroy(&pc->lock);

//    memset(pc, 0xba, sizeof(*pc)); // Zap for (hopefully) faster crash
    free (pc);

    //    printf(">>> %s\n", __func__);
}

void picpool_unref(picpool_ctl_t ** const pppc)
{
    int n;
    picpool_ctl_t * const pc = *pppc;

    if (pc == NULL)
        return;
    *pppc = NULL;

    n = atomic_fetch_sub(&pc->ref_count, 1) - 1;

    if (n != 0)
        return;

    picpool_delete(pc);
}

picpool_ctl_t * picpool_ref(picpool_ctl_t * const pc)
{
    atomic_fetch_add(&pc->ref_count, 1);
    return pc;
}

#if 0
static MMAL_BOOL_T vcz_pool_release_cb(MMAL_POOL_T * buf_pool, MMAL_BUFFER_HEADER_T *buf, void *userdata)
{
    picpool_ctl_t * const pc = userdata;
    vzc_subbuf_ent_t * const sb = buf->user_data;

    VLC_UNUSED(buf_pool);

//    printf("<<< %s\n", __func__);

    if (sb != NULL) {
        buf->user_data = NULL;
        pool_recycle(pc, sb->ent);
        picpool_release(pc);
        free(sb);
    }

//    printf(">>> %s\n", __func__);

    return MMAL_TRUE;
}
#endif

picpool_ctl_t * picpool_new(struct dmabufs_ctl * dbsc)
{
    picpool_ctl_t * pc;

    if (dbsc == NULL)
        return NULL;

    pc = calloc(1, sizeof(*pc));
    if (pc == NULL)
        return NULL;

    pc->max_n = 8;
    pc->dbsc = dmabufs_ctl_ref(dbsc);
    atomic_store(&pc->ref_count, 1);
    pthread_mutex_init(&pc->lock, NULL);

    return pc;
}
