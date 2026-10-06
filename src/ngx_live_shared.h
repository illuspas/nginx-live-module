
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_SHARED_H_INCLUDED_
#define _NGX_LIVE_SHARED_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"


/*
 * Reference-counted shared buffers, used for zero-copy fan-out
 * of media data to N subscribers.
 *
 * A single allocation is laid out as:
 *
 *     [uint32 refcount][ngx_chain_t][ngx_buf_t][payload]
 *
 * The refcount is stored in the NGX_LIVE_REFCOUNT_BYTES preceding
 * the ngx_chain_t.  The payload area starts with
 * NGX_LIVE_MAX_HEADER bytes of headroom so that protocol headers
 * (FLV tag header 11 bytes, RTMP chunk header up to 18 bytes in P2)
 * can be prepended in place before fan-out: b->pos -= hdr_len.
 */


#define NGX_LIVE_REFCOUNT_TYPE      uint32_t
#define NGX_LIVE_REFCOUNT_BYTES     sizeof(NGX_LIVE_REFCOUNT_TYPE)

/* FLV tag header 11 bytes now; RTMP chunk header up to 18 bytes in P2 */
#define NGX_LIVE_MAX_HEADER         18

/* payload area of one shared buffer block */
#define NGX_LIVE_BUF_SIZE           16384


/*
 * Refcount macros operate on the ngx_chain_t* (the count lives in the
 * NGX_LIVE_REFCOUNT_BYTES immediately preceding it).
 */

#define ngx_live_ref(ch)            (*((NGX_LIVE_REFCOUNT_TYPE *) (ch) - 1))
#define ngx_live_ref_set(ch, v)     (ngx_live_ref(ch) = v)
#define ngx_live_ref_get(ch)        (++ngx_live_ref(ch))
#define ngx_live_ref_put(ch)        (--ngx_live_ref(ch))


/* acquire a chain for an extra owner; must be paired with a free */
#define ngx_live_acquire_shared_chain(in)                                \
    ngx_live_ref_get(in)


/*
 * All functions take the application conf owning the buffer pool
 * (cacf->pool + cacf->free_bufs).  Buffers survive streams and are
 * recycled through the free chain.
 */

ngx_chain_t *ngx_live_alloc_shared_buf(ngx_live_core_app_conf_t *cacf);

void ngx_live_free_shared_chain(ngx_live_core_app_conf_t *cacf,
    ngx_chain_t *in);

/*
 * Append the data of "in" (from pos to last of every buf) to the
 * shared chain "head", allocating new blocks as needed.  Returns
 * "head" (which may be NULL on entry).  "in" is left untouched.
 */
ngx_chain_t *ngx_live_append_shared_bufs(ngx_live_core_app_conf_t *cacf,
    ngx_chain_t *head, ngx_chain_t *in);


#endif /* _NGX_LIVE_SHARED_H_INCLUDED_ */
