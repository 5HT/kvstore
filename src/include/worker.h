#ifndef KVS_WORKER_H
#define KVS_WORKER_H
#include "item.h"
#include "slab.h"
#include "spdk/blob.h"
#include "spdk/thread.h"

struct worker_context;
struct chunkmgr_worker_context;

struct worker_init_opts{
    uint32_t max_request_queue_size_per_worker;
    uint32_t max_io_pending_queue_size_per_worker;
    uint32_t reclaim_batch_size;
    uint32_t reclaim_percentage_threshold;

    uint32_t nb_shards;
    struct slab_shard *shard;

    struct spdk_blob_store *target;
    struct spdk_thread *meta_thread;

    struct chunkmgr_worker_context *chunkmgr_worker;

    //average chunks for each worker.
    uint64_t chunk_cache_water_mark;

    uint32_t core_id;
    
    uint32_t nb_reclaim_shards;
    uint32_t reclaim_shard_start_id;
};

struct worker_context* worker_alloc(struct worker_init_opts* opts);
void worker_start(struct worker_context* wctx);
void worker_destroy(struct worker_context* wctx);

/**
 * @brief When in recovering state, the work is not ready.
 */
bool worker_is_ready(struct worker_context* wctx);

//The item in kv_cb function is allocated temporarily. If you want to do something else, please copy it out 
//in kv_cb function

typedef void (*kv_cb)(void* ctx, struct kv_item* item, int kverrno);
typedef int (*modify_fn)(struct kv_item* item);

void worker_enqueue_get(struct worker_context* wctx,uint32_t shard,struct kv_item *item, kv_cb cb_fn, void* ctx);
void worker_enqueue_put(struct worker_context* wctx,uint32_t shard,struct kv_item *item, kv_cb cb_fn, void* ctx);
void worker_enqueue_delete(struct worker_context* wctx,uint32_t shard,struct kv_item *item, kv_cb cb_fn, void* ctx);

void worker_enqueue_rmw(struct worker_context* wctx,uint32_t shard,struct kv_item *item, modify_fn m_fn, kv_cb cb_fn, void* ctx);

struct worker_scan_result{
    uint32_t nb_items;
    uint32_t batch_size;
    struct kv_item* items[0];
};

typedef void (*scan_cb)(void* ctx, struct worker_scan_result* scan_res, int kverrno);

void worker_enqueue_seek(struct worker_context* wctx,struct kv_item *item, kv_cb cb_fn, void* ctx);
void worker_enqueue_first(struct worker_context* wctx, scan_cb cb_fn, uint32_t scan_batch, void* ctx);
void worker_enqueue_next(struct worker_context* wctx,struct kv_item *item, scan_cb cb_fn, uint32_t scan_batch, void* ctx);

struct worker_statistics{
    uint64_t chunk_hit_times;
    uint64_t chunk_miss_times;
    uint32_t nb_pending_reqs;
    uint32_t nb_pending_ios;
    uint64_t nb_used_chunks;
};
void worker_get_statistics(struct worker_context* wctx, struct worker_statistics* ws_out);

struct chunkmgr_worker_init_opts{
    uint32_t nb_business_workers;
    uint32_t core_id;
    uint64_t nb_max_cache_chunks;
    uint32_t nb_pages_per_chunk;
    struct worker_context **wctx_array;
};
struct chunkmgr_worker_context* chunkmgr_worker_alloc(struct chunkmgr_worker_init_opts *opts);
void chunkmgr_worker_start(void);
void chunkmgr_worker_destroy(void);


#endif
