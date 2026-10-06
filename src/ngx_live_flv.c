
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"
#include "ngx_live_shared.h"
#include "ngx_live_flv.h"
#include "ngx_live_core_module.h"


#define NGX_LIVE_FLV_STATE_HEADER    0
#define NGX_LIVE_FLV_STATE_TAGHDR    1
#define NGX_LIVE_FLV_STATE_BODY      2
#define NGX_LIVE_FLV_STATE_PREVTAG   3


ngx_int_t
ngx_live_flv_parser_init(ngx_live_flv_parser_t *parser,
    ngx_pool_t *pool, ngx_live_core_app_conf_t *cacf, ngx_log_t *log,
    ngx_live_flv_tag_pt on_tag, void *data)
{
    ngx_memzero(parser, sizeof(ngx_live_flv_parser_t));

    parser->pool = pool;
    parser->cacf = cacf;
    parser->log = log;
    parser->on_tag = on_tag;
    parser->data = data;

    parser->state = NGX_LIVE_FLV_STATE_HEADER;

    return NGX_OK;
}


ngx_int_t
ngx_live_flv_parse(ngx_live_flv_parser_t *parser, u_char *p, size_t len)
{
    size_t        need, take;
    ngx_buf_t     b;
    ngx_chain_t   cl;

    while (len) {

        switch (parser->state) {

        case NGX_LIVE_FLV_STATE_HEADER:

            need = NGX_LIVE_FLV_HEADER_SIZE - parser->small_len;
            take = ngx_min(need, len);

            ngx_memcpy(parser->small + parser->small_len, p, take);
            parser->small_len += take;
            p += take;
            len -= take;

            if (parser->small_len < NGX_LIVE_FLV_HEADER_SIZE) {
                return NGX_OK;
            }

            if (ngx_memcmp(parser->small, "FLV", 3) != 0) {
                ngx_log_error(NGX_LOG_ERR, parser->log, 0,
                              "live_flv: bad FLV signature");
                return NGX_ERROR;
            }

            /* version, flags and data offset are not validated */

            parser->state = NGX_LIVE_FLV_STATE_TAGHDR;
            parser->small_len = 0;
            break;

        case NGX_LIVE_FLV_STATE_TAGHDR:

            need = NGX_LIVE_FLV_TAGHDR_SIZE - parser->small_len;
            take = ngx_min(need, len);

            ngx_memcpy(parser->small + parser->small_len, p, take);
            parser->small_len += take;
            p += take;
            len -= take;

            if (parser->small_len < NGX_LIVE_FLV_TAGHDR_SIZE) {
                return NGX_OK;
            }

            parser->type = parser->small[0];

            parser->body_size = ((size_t) parser->small[1] << 16)
                                | ((size_t) parser->small[2] << 8)
                                | parser->small[3];

            parser->dts = ((uint32_t) parser->small[4] << 16)
                          | ((uint32_t) parser->small[5] << 8)
                          | (uint32_t) parser->small[6]
                          | ((uint32_t) parser->small[7] << 24);

            /* small[8..10]: stream id, always zero, not validated */

            parser->body = NULL;
            parser->body_received = 0;
            parser->small_len = 0;

            if (parser->body_size == 0) {
                /* zero-sized tag: skip straight to PreviousTagSize */
                parser->state = NGX_LIVE_FLV_STATE_PREVTAG;
                break;
            }

            parser->state = NGX_LIVE_FLV_STATE_BODY;
            break;

        case NGX_LIVE_FLV_STATE_BODY:

            take = ngx_min(parser->body_size - parser->body_received, len);

            /* wrap the input bytes and append them into the shared
             * chain; append copies the data, the input is untouched */

            ngx_memzero(&b, sizeof(ngx_buf_t));
            b.pos = p;
            b.last = p + take;
            b.memory = 1;

            cl.buf = &b;
            cl.next = NULL;

            parser->body = ngx_live_append_shared_bufs(parser->cacf,
                                                       parser->body, &cl);
            if (parser->body == NULL) {
                return NGX_ERROR;
            }

            parser->body_received += take;
            p += take;
            len -= take;

            if (parser->body_received < parser->body_size) {
                return NGX_OK;
            }

            if (parser->on_tag(parser->data, parser->type, parser->dts,
                               parser->body, parser->body_size)
                != NGX_OK)
            {
                /* ownership was moved to the callback */
                parser->body = NULL;
                return NGX_ERROR;
            }

            parser->body = NULL;
            parser->state = NGX_LIVE_FLV_STATE_PREVTAG;
            break;

        case NGX_LIVE_FLV_STATE_PREVTAG:

            need = NGX_LIVE_FLV_PREVTAG_SIZE - parser->small_len;
            take = ngx_min(need, len);

            ngx_memcpy(parser->small + parser->small_len, p, take);
            parser->small_len += take;
            p += take;
            len -= take;

            if (parser->small_len < NGX_LIVE_FLV_PREVTAG_SIZE) {
                return NGX_OK;
            }

            if (((size_t) parser->small[0] << 24
                 | (size_t) parser->small[1] << 16
                 | (size_t) parser->small[2] << 8
                 | (size_t) parser->small[3])
                != NGX_LIVE_FLV_TAGHDR_SIZE + parser->body_size)
            {
                ngx_log_error(NGX_LOG_ERR, parser->log, 0,
                              "live_flv: FLV PreviousTagSize mismatch "
                              "(tag type=%ui size=%uz)",
                              parser->type, parser->body_size);
                return NGX_ERROR;
            }

            parser->small_len = 0;
            parser->state = NGX_LIVE_FLV_STATE_TAGHDR;
            break;

        default:
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


void
ngx_live_flv_parser_cleanup(ngx_live_flv_parser_t *parser)
{
    if (parser->body) {
        ngx_live_free_shared_chain(parser->cacf, parser->body);
        parser->body = NULL;
    }
}
