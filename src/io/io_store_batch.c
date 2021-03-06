
#include <assert.h>
#include "io.h"
#include "slab.h"
#include "kverrno.h"
#include "hashmap.h"

#include "spdk/thread.h"

static void _store_pages_complete_cb(void*ctx, int kverrno);

static void
_process_cache_io(struct cache_io *cio,int kverrno){
    cio->cnt++;
    cio->kverrno = kverrno ? kverrno : -KV_ESUCCESS;

    if(cio->cnt==cio->nb_segments){
        //All the segments completes.
        cio->cb(cio->ctx,cio->kverrno);
        
        struct cache_io *i=NULL, *tmp=NULL;
        TAILQ_FOREACH_SAFE(i,&cio->cio_head,link,tmp){
            TAILQ_REMOVE(&cio->cio_head,i,link);

            //Release the pool first, then call the user callback.
            //the memory for cache_io i will not be freed. So I can 
            //still get the data.
            //In case that user call another put or get, the cache io will be
            //consumped reccurcively, inducing a pool-resource-not-enough error.
            //So I have to release the cache io firstly. 
            pool_release(cio->imgr->cache_io_pool,i);
            i->cb(i->ctx,cio->kverrno);
        }
        //this cio shall be lastly released!!
        pool_release(cio->imgr->cache_io_pool,cio);
    }
}

static void
_store_pages_phase2(struct page_io *pio){
    pio->imgr->nb_pending_io++;
    spdk_blob_io_write(pio->blob,pio->imgr->channel,pio->buf,pio->start_page,pio->len,
                           _store_pages_complete_cb,pio);
}

static void
_store_pages_complete_cb(void*ctx, int kverrno){
    struct page_io *pio = ctx;

    pio->imgr->nb_pending_io--;

    _process_cache_io(pio->cache_io,kverrno);
    if(pio->io_link){
        _store_pages_phase2(pio->io_link);
    }

    pool_release(pio->imgr->page_io_pool,pio);
}

void 
iomgr_store_pages_async(struct iomgr* imgr,
                            struct spdk_blob* blob, 
                            uint64_t key_prefix, 
                            uint8_t* buf,
                            uint64_t start_page, 
                            uint64_t nb_pages,                         
                            void(*cb)(void*ctx, int kverrno), 
                            void* ctx){

    assert( ((uint64_t)buf) % KVS_PAGE_SIZE==0 );

    struct cache_io *cio = NULL, *tmp = NULL;

    cio = pool_get(imgr->cache_io_pool);
    assert(cio!=NULL);
    cio->cb = cb;
    cio->ctx = ctx;
    cio->imgr = imgr;
    cio->start_page = start_page;
    cio->buf = buf;
    cio->nb_pages = nb_pages;
    cio->cnt=0;
    cio->nb_segments = nb_pages>1 ? 2 : 1;
    cio->blob = blob;
    _make_cache_key128(key_prefix,nb_pages,cio->key);

    //submit cio
    TAILQ_INSERT_TAIL(&imgr->pending_write_head,cio,link);
}

int iomgr_io_write_poll(struct iomgr* imgr){
    int events = 0;
    //Process the requests that covering the same pages
    map_t cmap = imgr->write_hash.cache_hash;
    struct cache_io *cio, *ctmp=NULL;
    TAILQ_FOREACH_SAFE(cio,&imgr->pending_write_head,link,ctmp){
        struct cache_io *val;
        hashmap_get(cmap,(uint8_t*)cio->key, sizeof(cio->key),&val);
        if(val){
            //They are writing the same pages, just link them.
            TAILQ_REMOVE(&imgr->pending_write_head,cio,link);
            TAILQ_INSERT_TAIL(&val->cio_head,cio,link);
        }
        else{
            TAILQ_INIT(&cio->cio_head);
            hashmap_put(cmap,(uint8_t*)cio->key,sizeof(cio->key),cio);
        }
    }

    TAILQ_HEAD(,page_io) pio_head;
    TAILQ_INIT(&pio_head);

    //Process the requests that have interleaved pages
    TAILQ_FOREACH_SAFE(cio,&imgr->pending_write_head,link,ctmp){
        TAILQ_REMOVE(&imgr->pending_write_head,cio,link);
        hashmap_remove(cmap,(uint8_t*)cio->key,sizeof(cio->key));

        struct page_io *pio1 = pool_get(imgr->page_io_pool);
        assert(pio1!=NULL);
        pio1->cache_io = cio;
        pio1->key = cio->key[0];
        pio1->imgr = imgr;
        pio1->io_link = NULL;
        pio1->buf = cio->buf;
        pio1->start_page = cio->start_page;
        pio1->len = cio->nb_segments==1 ? 1 : cio->nb_pages - 1;
        pio1->blob = cio->blob;

        if(cio->nb_segments==2){
            //Perform phase2 writing.
            struct page_io *pio2 = pool_get(imgr->page_io_pool);
            assert(pio2!=NULL);
            uint32_t pages = cio->nb_pages - 1;
            pio2->cache_io = cio;
            pio2->key = cio->key[0] + pages;
            pio2->imgr = imgr;
            pio2->io_link = NULL;
            pio2->buf = cio->buf + KVS_PAGE_SIZE*pages;
            pio2->start_page = cio->start_page + pages;
            pio2->len = 1;
            pio2->blob = cio->blob;
            
            //pio2 shall be processed after pio1
            pio1->io_link = pio2;
        }
        TAILQ_INSERT_TAIL(&pio_head,pio1,link);
    }

    struct page_io *pio, *ptmp = NULL;
    TAILQ_FOREACH_SAFE(pio,&pio_head,link,ptmp){
        TAILQ_REMOVE(&pio_head,pio,link);
        events++;
        imgr->nb_pending_io++;
        spdk_blob_io_write(pio->blob,pio->imgr->channel,pio->buf,pio->start_page,pio->len,
                           _store_pages_complete_cb,pio);
    }
    return events;
}
