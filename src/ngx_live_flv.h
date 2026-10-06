
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_FLV_H_INCLUDED_
#define _NGX_LIVE_FLV_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"


/*
 * Streaming FLV parser.
 *
 * Feeds arbitrary chunks of input bytes (e.g. decoded from an HTTP
 * chunked request body) and emits complete tags through the on_tag
 * callback.  Tag bodies are accumulated in reference-counted shared
 * buffers owned by the application conf, so the callback receives
 * a chain that can be cached and fanned out with zero extra copies.
 *
 *     header  13 bytes  "FLV" + version + flags + offset
 *     tag     11 bytes  TagHeader (type, size, timestamp)
 *             N  bytes  TagBody  (== RTMP message payload)
 *             4  bytes  PreviousTagSize0..N (strictly validated)
 */

#define NGX_LIVE_FLV_HEADER_SIZE    13
#define NGX_LIVE_FLV_TAGHDR_SIZE    11
#define NGX_LIVE_FLV_PREVTAG_SIZE   4


typedef struct ngx_live_flv_parser_s  ngx_live_flv_parser_t;


/*
 * Called for every complete tag.  "body" is a shared chain holding
 * the tag body only (no 11-byte header); its ownership moves to the
 * callback, which must free it (via the app conf) or hand it to the
 * stream core.  Zero-sized tags are skipped silently.
 */
typedef ngx_int_t (*ngx_live_flv_tag_pt)(void *data,
    ngx_uint_t type, uint32_t dts, ngx_chain_t *body, size_t size);


struct ngx_live_flv_parser_s {
    ngx_pool_t               *pool;    /* scratch allocations */
    ngx_live_core_app_conf_t *cacf;    /* shared buffer owner */
    ngx_log_t                *log;

    ngx_live_flv_tag_pt       on_tag;
    void                     *data;

    ngx_uint_t                state;
    u_char                    small[NGX_LIVE_FLV_HEADER_SIZE];
    size_t                    small_len;

    /* current tag */

    ngx_uint_t                type;
    size_t                    body_size;
    size_t                    body_received;
    uint32_t                  dts;
    ngx_chain_t              *body;
};


ngx_int_t ngx_live_flv_parser_init(ngx_live_flv_parser_t *parser,
    ngx_pool_t *pool, ngx_live_core_app_conf_t *cacf, ngx_log_t *log,
    ngx_live_flv_tag_pt on_tag, void *data);

/*
 * Consume len bytes of input.  Returns NGX_OK, or NGX_ERROR on
 * malformed stream (bad FLV signature or PreviousTagSize mismatch).
 */
ngx_int_t ngx_live_flv_parse(ngx_live_flv_parser_t *parser,
    u_char *p, size_t len);

/* release the pending (incomplete) tag body, if any */
void ngx_live_flv_parser_cleanup(ngx_live_flv_parser_t *parser);


#endif /* _NGX_LIVE_FLV_H_INCLUDED_ */
