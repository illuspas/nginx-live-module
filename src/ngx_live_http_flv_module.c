
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "ngx_live.h"
#include "ngx_live_shared.h"
#include "ngx_live_packet.h"
#include "ngx_live_flv.h"
#include "ngx_live_stream.h"
#include "ngx_live_access_module.h"
#include "ngx_live_core_module.h"


typedef struct ngx_live_flv_player_s    ngx_live_flv_player_t;
typedef struct ngx_live_flv_out_node_s  ngx_live_flv_out_node_t;


static ngx_int_t ngx_live_flv_handler(ngx_http_request_t *r);
static ngx_int_t ngx_live_flv_publish(ngx_http_request_t *r,
    ngx_live_core_app_conf_t *cacf, ngx_str_t *name);
static void ngx_live_flv_publish_read(ngx_http_request_t *r);
static ngx_int_t ngx_live_flv_process_input(ngx_http_request_t *r,
    u_char *p, size_t len);
static ngx_int_t ngx_live_flv_on_tag(void *data, ngx_uint_t type,
    uint32_t dts, ngx_chain_t *body, size_t size);
static void ngx_live_flv_publisher_finish(ngx_http_request_t *r,
    ngx_int_t status);
static void ngx_live_flv_publisher_cleanup(void *data);

/* playback (S4) */

static ngx_int_t ngx_live_flv_play(ngx_http_request_t *r,
    ngx_live_core_app_conf_t *cacf, ngx_live_stream_t *stream,
    ngx_str_t *name);
static void ngx_live_flv_player_write(ngx_http_request_t *r);
static ngx_int_t ngx_live_flv_player_send(void *sess,
    ngx_live_packet_t *pkt);
static void ngx_live_flv_player_close(void *sess);
static void ngx_live_flv_player_put_raw(ngx_live_flv_player_t *p,
    u_char *data, size_t len, ngx_int_t last);
static void ngx_live_flv_player_enqueue(ngx_live_flv_player_t *p,
    ngx_live_packet_t *pkt);
static void ngx_live_flv_player_put_node(ngx_live_flv_player_t *p,
    ngx_live_flv_out_node_t *node);
static void ngx_live_flv_player_flush(ngx_live_flv_player_t *p);
static void ngx_live_flv_player_drain(ngx_live_flv_player_t *p,
    ngx_int_t all);
static void ngx_live_flv_player_finish(ngx_live_flv_player_t *p,
    ngx_int_t status);
static void ngx_live_flv_player_cleanup(void *data);

static void *ngx_live_flv_create_loc_conf(ngx_conf_t *cf);
static char *ngx_live_flv_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *conf);
static char *ngx_live_flv(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_live_core_app_conf_t *ngx_live_flv_find_application(
    ngx_str_t *app);
static ngx_int_t ngx_live_flv_safe_name(ngx_str_t *name);
static ngx_int_t ngx_live_flv_test_expect(ngx_http_request_t *r);


/* chunked decoding states */

#define NGX_LIVE_CHUNK_SIZE      0
#define NGX_LIVE_CHUNK_DATA      1
#define NGX_LIVE_CHUNK_AFTER     2   /* CRLF after chunk data */
#define NGX_LIVE_CHUNK_TRAILER   3   /* trailer headers until empty line */
#define NGX_LIVE_CHUNK_DONE      4


typedef struct {
    ngx_flag_t      flv;    /* live_flv on|off */
} ngx_live_flv_loc_conf_t;


typedef struct ngx_live_flv_player_s  ngx_live_flv_player_t;
typedef struct ngx_live_flv_out_node_s  ngx_live_flv_out_node_t;


typedef struct {
    ngx_http_request_t       *r;
    ngx_live_core_app_conf_t *cacf;
    ngx_live_stream_t        *stream;

    ngx_live_flv_parser_t     parser;

    /* request body chunked decoding */
    ngx_uint_t                chunk_state;
    uint64_t                  chunk_size;
    uint64_t                  chunk_received;

    /* remaining body bytes for Content-Length requests */
    off_t                     body_left;

    /* accumulating the current size/terminator line */
    u_char                    line[256];
    size_t                    line_len;

    unsigned                  body_done:1;   /* request body fully received */
    unsigned                  done:1;        /* request finished */
} ngx_live_flv_ctx_t;


typedef struct {
    ngx_live_flv_ctx_t       *ctx;
} ngx_live_flv_cleanup_t;


/*
 * One queued output "node": the per-session clone of one shared FLV
 * tag (chain/buf structures cloned from the pool, data pointers
 * shared) plus the 4-byte PreviousTagSize trailer.  Node chains are
 * linked into the session output chain; the write filter advances
 * cloned buf positions, and a node whose tail link ran dry releases
 * its shared reference.
 */

typedef struct ngx_live_flv_out_node_s {
    struct ngx_live_flv_out_node_s *next;
    ngx_chain_t                    *shared;   /* NULL for header/trailer */
    ngx_chain_t                    *chain;    /* first cloned link */
    ngx_chain_t                    *tail;     /* last cloned link */
    size_t                          size;     /* payload bytes queued */
} ngx_live_flv_out_node_t;


struct ngx_live_flv_player_s {
    ngx_live_subscriber_t    sub;       /* keep first */

    ngx_http_request_t      *r;
    ngx_live_core_app_conf_t *cacf;
    ngx_live_stream_t        *stream;

    ngx_live_flv_out_node_t *node_head; /* queued tags */
    ngx_live_flv_out_node_t *node_tail;
    ngx_chain_t             *out_head;  /* linked output chain */
    size_t                   out_size;  /* pending payload bytes */

    ngx_uint_t               dropped;   /* dropped packets (backpressure) */
    ngx_msec_t               last_progress;

    /*
     * Subscribed without a cached keyframe start point (empty cache
     * or gop_cache off): video frames are held until the stream's
     * next keyframe, so playback never begins mid-GOP.
     */
    unsigned                 wait_key:1;

    unsigned                 closed:1;
    unsigned                 detaching:1;
    unsigned                 busy:1;    /* out_head submitted to the
                                         * write filter (r->out owns it) */
    unsigned                 ending:1;  /* chunked trailer queued */
};


typedef struct {
    ngx_live_flv_player_t   *player;
} ngx_live_flv_player_cleanup_t;


/* FLV file header: audio+video flags, offset 9, PreviousTagSize0 */
static u_char ngx_live_flv_file_header[] = {
    'F', 'L', 'V', 0x01, 0x05, 0x00, 0x00, 0x00, 0x09,
    0x00, 0x00, 0x00, 0x00
};


#define NGX_LIVE_FLV_OUT_LIMIT      1024 * 1024   /* 1M pending bytes */
#define NGX_LIVE_FLV_STALL_MS       3000          /* slow consumer cut */
#define NGX_LIVE_FLV_SEND_TIMEOUT   60000


#define NGX_LIVE_FLV_BUF_SIZE     16384
#define NGX_LIVE_FLV_TIMEOUT      60000


static ngx_command_t  ngx_live_flv_commands[] = {

    { ngx_string("live_flv"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_live_flv,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_live_flv_loc_conf_t, flv),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_live_flv_module_ctx = {
    NULL,                               /* preconfiguration */
    NULL,                               /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */

    ngx_live_flv_create_loc_conf,       /* create location configuration */
    ngx_live_flv_merge_loc_conf         /* merge location configuration */
};


ngx_module_t  ngx_live_http_flv_module = {
    NGX_MODULE_V1,
    &ngx_live_flv_module_ctx,           /* module context */
    ngx_live_flv_commands,              /* module directives */
    NGX_HTTP_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_live_flv_handler(ngx_http_request_t *r)
{
    u_char                      *p;
    ngx_int_t                    rc;
    ngx_str_t                    app, name;
    ngx_live_core_app_conf_t    *cacf;

    if (r->method & NGX_HTTP_POST) {
        goto publish;
    }

    if (!(r->method & NGX_HTTP_GET)) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    /* uri: /app/name.flv */

    if (r->uri.len < 2 || r->uri.data[0] != '/') {
        goto bad;
    }

    app.data = r->uri.data + 1;

    p = ngx_strlchr(app.data, r->uri.data + r->uri.len, '/');
    if (p == NULL) {
        goto bad;
    }

    app.len = p - app.data;

    name.data = p + 1;
    name.len = r->uri.data + r->uri.len - name.data;

    if (name.len < 5
        || ngx_strncasecmp(name.data + name.len - 4, (u_char *) ".flv", 4) != 0)
    {
        goto bad;
    }

    name.len -= 4;

    cacf = ngx_live_flv_find_application(&app);
    if (cacf == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "live_flv: application \"%V\" not configured", &app);
        return NGX_HTTP_NOT_FOUND;
    }

    if (cacf->live == 0) {
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    if (ngx_live_access_permit(cacf, r->connection, NGX_LIVE_ACCESS_PLAY)
        != NGX_OK)
    {
        return NGX_HTTP_FORBIDDEN;
    }

    if (ngx_live_flv_safe_name(&name) == 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "live_flv: unsafe stream name in \"%V\"", &r->uri);
        return NGX_HTTP_FORBIDDEN;
    }

    if (r->http_version < NGX_HTTP_VERSION_11) {
        /* HTTP-FLV needs chunked transfer, HTTP/1.0 cannot do it */
        return NGX_HTTP_VERSION_NOT_SUPPORTED;
    }

    {
        ngx_live_stream_t  **sp;

        sp = ngx_live_find_stream(cacf, &name, 0);
        if (sp == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "live_flv: stream \"%V\" not found", &name);
            return NGX_HTTP_NOT_FOUND;
        }

        return ngx_live_flv_play(r, cacf, *sp, &name);
    }

publish:

    /* uri: /app/name[.flv] */

    if (r->uri.len < 2 || r->uri.data[0] != '/') {
        goto bad;
    }

    app.data = r->uri.data + 1;

    p = ngx_strlchr(app.data, r->uri.data + r->uri.len, '/');
    if (p == NULL) {
        goto bad;
    }

    app.len = p - app.data;

    name.data = p + 1;
    name.len = r->uri.data + r->uri.len - name.data;

    if (name.len > 4
        && ngx_strncasecmp(name.data + name.len - 4, (u_char *) ".flv", 4) == 0)
    {
        name.len -= 4;
    }

    cacf = ngx_live_flv_find_application(&app);
    if (cacf == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "live_flv: application \"%V\" not configured", &app);
        return NGX_HTTP_NOT_FOUND;
    }

    if (cacf->live == 0) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "live_flv: application \"%V\" has live off", &app);
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    if (ngx_live_access_permit(cacf, r->connection,
                               NGX_LIVE_ACCESS_PUBLISH) != NGX_OK)
    {
        return NGX_HTTP_FORBIDDEN;
    }

    if (!ngx_live_flv_safe_name(&name)) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "live_flv: unsafe stream name in \"%V\"", &r->uri);
        return NGX_HTTP_FORBIDDEN;
    }

    if (!r->headers_in.chunked && r->headers_in.content_length_n < 0) {
        return NGX_HTTP_LENGTH_REQUIRED;
    }

    return ngx_live_flv_publish(r, cacf, &name);

bad:

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "live_flv: bad uri \"%V\"", &r->uri);

    return NGX_HTTP_BAD_REQUEST;
}


static ngx_int_t
ngx_live_flv_publish(ngx_http_request_t *r, ngx_live_core_app_conf_t *cacf,
    ngx_str_t *name)
{
    ngx_live_flv_ctx_t     *ctx;
    ngx_live_flv_cleanup_t *cln;
    ngx_live_stream_t     **sp;
    ngx_http_cleanup_t     *hcln;

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_live_flv_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_live_http_flv_module);

    ctx->r = r;
    ctx->cacf = cacf;

    sp = ngx_live_find_stream(cacf, name, 1);
    if (sp == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ngx_live_stream_publish(*sp, ctx) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "live_flv: stream \"%V\" already publishing", name);
        return NGX_HTTP_CONFLICT;
    }

    ctx->stream = *sp;

    ngx_live_flv_parser_init(&ctx->parser, r->pool, cacf,
                             r->connection->log, ngx_live_flv_on_tag, ctx);

    ctx->chunk_state = NGX_LIVE_CHUNK_SIZE;
    ctx->body_left = -1;

    if (!r->headers_in.chunked) {
        /* the 411 guard above ensures a positive Content-Length */
        ctx->body_left = r->headers_in.content_length_n;
    }

    /* the request outlives the connection phases: keep it counted
     * and finish it when the body is fully consumed or the peer
     * goes away */

    hcln = ngx_http_cleanup_add(r, sizeof(ngx_live_flv_cleanup_t));
    if (hcln == NULL) {
        ngx_live_stream_close(ctx->stream);
        ctx->stream = NULL;
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    cln = hcln->data;
    cln->ctx = ctx;
    hcln->handler = ngx_live_flv_publisher_cleanup;

    /*
     * Interim 100 Continue: we take over body reading ourselves,
     * so the framework (ngx_http_read_client_request_body) never
     * runs and never answers an Expect header.  curl sends it for
     * large request bodies.
     */

    if (ngx_live_flv_test_expect(r) != NGX_OK) {
        ngx_live_stream_close(ctx->stream);
        ctx->stream = NULL;
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    r->read_event_handler = ngx_live_flv_publish_read;
    r->write_event_handler = ngx_http_request_empty_handler;

    r->main->count++;

    ngx_add_timer(r->connection->read, NGX_LIVE_FLV_TIMEOUT);

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "live_flv: publish started, app=%V stream=%V",
                  &cacf->name, name);

    /* consume any body bytes that arrived together with the request
     * headers; they may live in a large header buffer, not c->buffer */

    if (r->header_in && r->header_in->pos < r->header_in->last) {

        if (ngx_live_flv_process_input(r, r->header_in->pos,
                                       r->header_in->last - r->header_in->pos)
            != NGX_OK)
        {
            ngx_live_flv_publisher_finish(r, NGX_HTTP_BAD_REQUEST);
            return NGX_OK;
        }

        r->header_in->pos = r->header_in->last;

        if (ctx->body_done || ctx->chunk_state == NGX_LIVE_CHUNK_DONE) {
            ngx_live_flv_publisher_finish(r, NGX_HTTP_OK);
            return NGX_OK;
        }
    }

    /*
     * More body bytes may already sit in the socket buffer (the
     * read event that delivered the headers does not re-fire for
     * them).  Drain them now, like ngx_http_read_client_request_body
     * does; ngx_live_flv_publish_read() arms the event for the rest.
     */

    ngx_live_flv_publish_read(r);

    if (ctx->done) {
        return NGX_OK;
    }

    return NGX_DONE;
}


static void
ngx_live_flv_publish_read(ngx_http_request_t *r)
{
    u_char             buf[NGX_LIVE_FLV_BUF_SIZE];
    ngx_int_t          n;
    ngx_event_t       *rev;
    ngx_connection_t  *c;

    c = r->connection;
    rev = c->read;

    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "live_flv: publish timed out");
        ngx_live_flv_publisher_finish(r, NGX_HTTP_REQUEST_TIME_OUT);
        return;
    }

    for ( ;; ) {

        n = ngx_recv(c, buf, sizeof(buf));

        if (n == NGX_AGAIN) {
            break;
        }

        if (n == NGX_ERROR) {
            ngx_live_flv_publisher_finish(r, NGX_HTTP_BAD_REQUEST);
            return;
        }

        if (n == 0) {
            /* peer closed: end of the FLV stream */
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "live_flv: publish eof");
            ngx_live_flv_publisher_finish(r, NGX_HTTP_OK);
            return;
        }

        ngx_add_timer(rev, NGX_LIVE_FLV_TIMEOUT);

        if (ngx_live_flv_process_input(r, buf, (size_t) n) != NGX_OK) {
            ngx_live_flv_publisher_finish(r, NGX_HTTP_BAD_REQUEST);
            return;
        }

        {
            ngx_live_flv_ctx_t *ctx;

            ctx = ngx_http_get_module_ctx(r, ngx_live_http_flv_module);

            if (ctx->body_done || ctx->chunk_state == NGX_LIVE_CHUNK_DONE) {
                ngx_live_flv_publisher_finish(r, NGX_HTTP_OK);
                return;
            }
        }
    }

    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
        ngx_live_flv_publisher_finish(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
    }
}


static ngx_int_t ngx_live_flv_chunk_size(ngx_live_flv_ctx_t *ctx);


/*
 * Run the raw request body bytes through chunked decoding (when
 * needed) and the FLV parser.
 */

static ngx_int_t
ngx_live_flv_process_input(ngx_http_request_t *r, u_char *p, size_t len)
{
    size_t              take;
    ngx_uint_t          i;
    ngx_live_flv_ctx_t *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_live_http_flv_module);

    if (!r->headers_in.chunked) {
        take = len;

        if (ctx->body_left >= 0) {
            if ((off_t) len > ctx->body_left) {
                take = ctx->body_left;    /* next pipelined request */
            }

            ctx->body_left -= take;

            if (ctx->body_left == 0) {
                ctx->body_done = 1;
            }
        }

        return ngx_live_flv_parse(&ctx->parser, p, take);
    }

    while (len) {

        if (ctx->chunk_state == NGX_LIVE_CHUNK_DONE) {
            return NGX_OK;
        }

        if (ctx->chunk_state == NGX_LIVE_CHUNK_DATA) {

            take = (size_t) ngx_min(ctx->chunk_size - ctx->chunk_received,
                                    (uint64_t) len);

            if (take == 0) {
                ctx->chunk_state = NGX_LIVE_CHUNK_AFTER;
                ctx->line_len = 0;
                continue;
            }

            if (ngx_live_flv_parse(&ctx->parser, p, take) != NGX_OK) {
                return NGX_ERROR;
            }

            ctx->chunk_received += take;
            p += take;
            len -= take;

            if (ctx->chunk_received == ctx->chunk_size) {
                ctx->chunk_state = NGX_LIVE_CHUNK_AFTER;
                ctx->line_len = 0;
            }

            continue;
        }

        /*
         * SIZE / AFTER / TRAILER states accumulate one LF-terminated
         * line at a time.
         */

        for (i = 0; i < len; i++) {

            if (p[i] != '\n') {
                if (ctx->line_len >= sizeof(ctx->line)) {
                    goto bad;
                }
                ctx->line[ctx->line_len++] = p[i];
                continue;
            }

            /* complete line accumulated in ctx->line */

            switch (ctx->chunk_state) {

            case NGX_LIVE_CHUNK_SIZE:

                if (ngx_live_flv_chunk_size(ctx) != NGX_OK) {
                    goto bad;
                }

                if (ctx->chunk_size == 0) {
                    ctx->chunk_state = NGX_LIVE_CHUNK_TRAILER;

                } else {
                    ctx->chunk_state = NGX_LIVE_CHUNK_DATA;
                }

                ctx->chunk_received = 0;
                break;

            case NGX_LIVE_CHUNK_AFTER:

                ctx->chunk_state = NGX_LIVE_CHUNK_SIZE;
                ctx->chunk_size = 0;
                break;

            case NGX_LIVE_CHUNK_TRAILER:

                /* empty line ends the chunked body */

                if (ctx->line_len == 0
                    || (ctx->line_len == 1 && ctx->line[0] == '\r'))
                {
                    ctx->chunk_state = NGX_LIVE_CHUNK_DONE;
                }
                break;

            default:
                goto bad;
            }

            ctx->line_len = 0;
            p += i + 1;
            len -= i + 1;

            goto line_done;
        }

        return NGX_OK;    /* no LF: line continues in the next buffer */

line_done:

        continue;    /* re-evaluate the state on the outer loop */
    }

    return NGX_OK;

bad:

    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                  "live_flv: bad chunked encoding");

    return NGX_ERROR;
}


/*
 * Parse the accumulated chunk-size line: hex size up to ';' or CR.
 */

static ngx_int_t
ngx_live_flv_chunk_size(ngx_live_flv_ctx_t *ctx)
{
    u_char   c;
    size_t   i, n;

    ctx->chunk_size = 0;
    n = ctx->line_len;

    for (i = 0; i < n; i++) {

        c = ctx->line[i];

        if (c == ';' || c == '\r') {
            break;
        }

        if (ctx->chunk_size > 0xfffffffffffffffULL / 16) {
            return NGX_ERROR;
        }

        if (c >= '0' && c <= '9') {
            ctx->chunk_size = ctx->chunk_size * 16 + (c - '0');

        } else if (c >= 'a' && c <= 'f') {
            ctx->chunk_size = ctx->chunk_size * 16 + (c - 'a' + 10);

        } else if (c >= 'A' && c <= 'F') {
            ctx->chunk_size = ctx->chunk_size * 16 + (c - 'A' + 10);

        } else {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_live_flv_on_tag(void *data, ngx_uint_t type, uint32_t dts,
    ngx_chain_t *body, size_t size)
{
    ngx_live_packet_t   pkt;
    ngx_live_flv_ctx_t *ctx = data;

    ngx_memzero(&pkt, sizeof(ngx_live_packet_t));

    pkt.codec_type = type;
    pkt.dts = dts;
    pkt.size = size;
    pkt.chain = body;

    if (ngx_live_packet_identify(&pkt) != NGX_OK) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->r->connection->log, 0,
                       "live_flv: malformed tag (type=%ui) dropped", type);
        ngx_live_free_shared_chain(ctx->cacf, body);
        return NGX_OK;
    }

    ngx_log_debug4(NGX_LOG_DEBUG_HTTP, ctx->r->connection->log, 0,
                   "live_flv: tag type=%M flags=%ui dts=%uD size=%uz",
                   (ngx_msec_t) type, pkt.flags, pkt.dts, size);

    /* prepend the 11-byte FLV tag header; the shared buffer has
     * headroom, so this is a zero-copy packaging step */

    ngx_live_packet_prepend_tag_header(&pkt);

    ngx_live_stream_packet(ctx->stream, &pkt);

    /* release the publisher's own reference; caches and subscribers
     * hold their acquired references */

    ngx_live_free_shared_chain(ctx->cacf, pkt.chain);

    return NGX_OK;
}


static void
ngx_live_flv_publisher_finish(ngx_http_request_t *r, ngx_int_t status)
{
    ngx_int_t          rc;
    ngx_buf_t          b;
    ngx_chain_t        out;
    ngx_live_flv_ctx_t *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_live_http_flv_module);

    if (ctx == NULL || ctx->done) {
        return;
    }

    ctx->done = 1;

    if (ctx->stream) {
        ngx_live_stream_close(ctx->stream);
        ctx->stream = NULL;
    }

    ngx_live_flv_parser_cleanup(&ctx->parser);

    ngx_del_timer(r->connection->read);

    if (status >= NGX_HTTP_SPECIAL_RESPONSE) {
        /* let the framework render and flush the error page */
        ngx_http_finalize_request(r, status);
        return;
    }

    /*
     * 200: a headers-only response never reaches the wire (the
     * write filter holds it in postpone_output, and the chunked
     * terminal never comes).  Send an empty last_buf instead so
     * the chunked filter emits the trailer and everything flushes.
     */

    r->headers_out.status = status;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK) {
        ngx_http_finalize_request(r, rc);
        return;
    }

    ngx_memzero(&b, sizeof(ngx_buf_t));
    b.sync = 1;
    b.last_buf = 1;

    out.buf = &b;
    out.next = NULL;

    ngx_http_finalize_request(r, ngx_http_output_filter(r, &out));
}


static void
ngx_live_flv_publisher_cleanup(void *data)
{
    ngx_live_flv_cleanup_t *cln = data;

    if (cln->ctx && cln->ctx->stream) {
        ngx_live_stream_close(cln->ctx->stream);
        cln->ctx->stream = NULL;
    }

    if (cln->ctx) {
        ngx_live_flv_parser_cleanup(&cln->ctx->parser);
    }
}


/*
 * Playback (S4)
 */

static ngx_int_t
ngx_live_flv_play(ngx_http_request_t *r, ngx_live_core_app_conf_t *cacf,
    ngx_live_stream_t *stream, ngx_str_t *name)
{
    ngx_int_t                      rc;
    ngx_http_cleanup_t            *hcln;
    ngx_live_flv_player_t         *p;
    ngx_live_flv_player_cleanup_t *pcln;
    ngx_live_gop_node_t           *node;
    ngx_queue_t                   *item;

    p = ngx_pcalloc(r->pool, sizeof(ngx_live_flv_player_t));
    if (p == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(r, p, ngx_live_http_flv_module);

    p->r = r;
    p->cacf = cacf;
    p->stream = stream;
    p->last_progress = ngx_current_msec;

    p->sub.sess = p;
    p->sub.send = ngx_live_flv_player_send;
    p->sub.close = ngx_live_flv_player_close;

    hcln = ngx_http_cleanup_add(r, sizeof(ngx_live_flv_player_cleanup_t));
    if (hcln == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    pcln = hcln->data;
    pcln->player = p;
    hcln->handler = ngx_live_flv_player_cleanup;

    /* headers: keep content_length_n unset so the chunked filter
     * takes over the framing */

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_type_len = sizeof("video/x-flv") - 1;
    r->headers_out.content_type.len = sizeof("video/x-flv") - 1;
    r->headers_out.content_type.data = (u_char *) "video/x-flv";

    r->main->count++;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK) {
        r->main->count--;
        return rc;
    }

    /* startup sequence: FLV header, meta, codec headers, GOP cache */

    ngx_live_flv_player_put_raw(p, ngx_live_flv_file_header,
                                sizeof(ngx_live_flv_file_header), 0);

    if (stream->meta) {
        ngx_live_flv_player_enqueue(p, stream->meta);
    }

    if (stream->audio_header) {
        ngx_live_flv_player_enqueue(p, stream->audio_header);
    }

    if (stream->video_header) {
        ngx_live_flv_player_enqueue(p, stream->video_header);
    }

    for (item = ngx_queue_head(&stream->gop_cache);
         item != ngx_queue_sentinel(&stream->gop_cache);
         item = ngx_queue_next(item))
    {
        node = ngx_queue_data(item, ngx_live_gop_node_t, queue);
        ngx_live_flv_player_enqueue(p, &node->pkt);
    }

    /* subscribe before flushing so no packet is missed in between */

    /*
     * When no cached GOP exists (gop_cache off, or the current GOP
     * overflowed the limit), live packets start mid-GOP: hold video
     * frames until the next keyframe so the decoder gets a clean
     * random access point.  Audio goes out as-is.
     */

    if (ngx_queue_empty(&stream->gop_cache)) {
        p->wait_key = 1;
    }

    ngx_live_stream_subscribe(stream, &p->sub);

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "live_flv: play started, app=%V stream=%V "
                  "(gop=%ui subscribers=%ui)",
                  &cacf->name, name, stream->gop_count,
                  stream->nsubscribers);

    r->write_event_handler = ngx_live_flv_player_write;
    r->read_event_handler = ngx_http_test_reading;

    ngx_live_flv_player_flush(p);

    return NGX_DONE;
}


static void
ngx_live_flv_player_write(ngx_http_request_t *r)
{
    ngx_event_t           *wev;
    ngx_connection_t      *c;
    ngx_live_flv_player_t *p;

    p = ngx_http_get_module_ctx(r, ngx_live_http_flv_module);
    c = r->connection;
    wev = c->write;

    if (wev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "live_flv: play send timed out");
        ngx_live_flv_player_finish(p, NGX_HTTP_REQUEST_TIME_OUT);
        return;
    }

    ngx_live_flv_player_flush(p);
}


static ngx_int_t
ngx_live_flv_player_send(void *sess, ngx_live_packet_t *pkt)
{
    ngx_live_flv_player_t *p = sess;

    if (p->closed) {
        return NGX_OK;
    }

    /* mid-GOP start: hold video until the stream reaches a keyframe */

    if (p->wait_key) {
        if (pkt->flags == NGX_LIVE_FLAG_VIDEO_KEY) {
            p->wait_key = 0;

        } else if (pkt->flags == NGX_LIVE_FLAG_VIDEO_INTER) {
            return NGX_OK;
        }
    }

    /* backpressure: when the queue overflows, drop until the next
     * keyframe arrives, then restart from it */

    if (p->out_size > NGX_LIVE_FLV_OUT_LIMIT) {

        if (pkt->flags == NGX_LIVE_FLAG_VIDEO_KEY) {
            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, p->r->connection->log, 0,
                           "live_flv: resync at keyframe, dropped=%ui",
                           p->dropped);
            ngx_live_flv_player_drain(p, 1);

        } else if (pkt->flags == NGX_LIVE_FLAG_AUDIO
                   || pkt->flags == NGX_LIVE_FLAG_VIDEO_INTER)
        {
            p->dropped++;
            return NGX_OK;
        }
    }

    ngx_live_flv_player_enqueue(p, pkt);

    ngx_live_flv_player_flush(p);

    return NGX_OK;
}


static void
ngx_live_flv_player_close(void *sess)
{
    ngx_live_flv_player_t *p = sess;

    if (p->closed) {
        return;
    }

    /* the stream core has already unlinked the subscriber node */

    p->detaching = 1;

    /*
     * The stream ended: queue the chunked trailer and finish once
     * it has actually gone out (flush() completes the request when
     * everything queued, trailer included, has been sent).
     */

    ngx_live_flv_player_put_raw(p, NULL, 0, 1);
    p->ending = 1;

    ngx_live_flv_player_flush(p);
}


/* Queue one raw buffer (FLV file header, or the empty last_buf that
 * terminates a chunked response).  Data is static or none. */

static void
ngx_live_flv_player_put_raw(ngx_live_flv_player_t *p, u_char *data,
    size_t len, ngx_int_t last)
{
    ngx_live_flv_out_node_t *node;
    ngx_chain_t             *cl;
    ngx_buf_t               *b;

    node = ngx_pcalloc(p->r->pool, sizeof(ngx_live_flv_out_node_t));
    if (node == NULL) {
        return;
    }

    cl = ngx_alloc_chain_link(p->r->pool);
    if (cl == NULL) {
        return;
    }

    b = ngx_pcalloc(p->r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
        return;
    }

    if (data) {
        b->pos = data;
        b->last = data + len;
        b->memory = 1;
    }

    b->flush = 1;
    b->last_buf = last;
    b->sync = last;

    cl->buf = b;
    cl->next = NULL;

    node->chain = cl;
    node->tail = cl;
    node->size = len;

    ngx_live_flv_player_put_node(p, node);
}


/* Queue one packet: clone the shared tag chain (buf structures only)
 * and append the 4-byte PreviousTagSize trailer. */

static void
ngx_live_flv_player_enqueue(ngx_live_flv_player_t *p, ngx_live_packet_t *pkt)
{
    size_t                   size;
    u_char                  *b;
    ngx_buf_t               *nb;
    ngx_chain_t             *cl, *first = NULL, *last_cl = NULL, *prev_cl;
    ngx_live_flv_out_node_t *node;

    size = 0;

    /* clone every shared buffer of the tag */

    for (cl = pkt->chain; cl; cl = cl->next) {

        nb = ngx_pcalloc(p->r->pool, sizeof(ngx_buf_t));
        if (nb == NULL) {
            return;
        }

        nb->pos = cl->buf->pos;
        nb->last = cl->buf->last;
        nb->start = cl->buf->start;
        nb->end = cl->buf->end;
        nb->memory = 1;
        nb->flush = 1;

        prev_cl = ngx_alloc_chain_link(p->r->pool);
        if (prev_cl == NULL) {
            return;
        }

        prev_cl->buf = nb;
        prev_cl->next = NULL;

        if (last_cl) {
            last_cl->next = prev_cl;
        } else {
            first = prev_cl;
        }
        last_cl = prev_cl;

        size += cl->buf->last - cl->buf->pos;
    }

    if (first == NULL) {
        return;
    }

    /* PreviousTagSize = 11-byte header + body */

    prev_cl = ngx_alloc_chain_link(p->r->pool);
    if (prev_cl == NULL) {
        return;
    }

    nb = ngx_pcalloc(p->r->pool, sizeof(ngx_buf_t));
    if (nb == NULL) {
        return;
    }

    {
        u_char  buf[4];

        buf[0] = (u_char) (size >> 24);
        buf[1] = (u_char) (size >> 16);
        buf[2] = (u_char) (size >> 8);
        buf[3] = (u_char) size;

        b = ngx_pnalloc(p->r->pool, 4);
        if (b == NULL) {
            return;
        }

        ngx_memcpy(b, buf, 4);

        nb->pos = b;
        nb->last = b + 4;
        nb->memory = 1;
        nb->flush = 1;
    }

    prev_cl->buf = nb;
    prev_cl->next = NULL;

    last_cl->next = prev_cl;

    node = ngx_pcalloc(p->r->pool, sizeof(ngx_live_flv_out_node_t));
    if (node == NULL) {
        return;
    }

    node->shared = pkt->chain;
    node->chain = first;
    node->tail = prev_cl;
    node->size = size;

    ngx_live_acquire_shared_chain(pkt->chain);

    ngx_live_flv_player_put_node(p, node);

    p->out_size += size;
}


static void
ngx_live_flv_player_put_node(ngx_live_flv_player_t *p,
    ngx_live_flv_out_node_t *node)
{
    if (p->node_tail) {
        p->node_tail->tail->next = node->chain;
        p->node_tail->next = node;
        p->node_tail = node;

    } else {
        p->node_head = node;
        p->node_tail = node;
        p->out_head = node->chain;
    }
}


static void
ngx_live_flv_player_flush(ngx_live_flv_player_t *p)
{
    ngx_int_t                 rc;
    ngx_chain_t              *out;
    ngx_connection_t         *c;
    ngx_http_core_loc_conf_t *clcf;

    if (p->closed) {
        return;
    }

    c = p->r->connection;
    rc = NGX_OK;
    for ( ;; ) {

        /* release fully sent links from the head of the chain */

        ngx_live_flv_player_drain(p, 0);

        /* slow consumer: queue over the limit and stalled for a while */

        if (p->out_size > NGX_LIVE_FLV_OUT_LIMIT
            && ngx_current_msec - p->last_progress > NGX_LIVE_FLV_STALL_MS)
        {
            ngx_log_error(NGX_LOG_INFO, c->log, 0,
                          "live_flv: slow consumer dropped "
                          "(out=%uz dropped=%ui)",
                          p->out_size, p->dropped);
            ngx_live_flv_player_finish(p, NGX_HTTP_CLIENT_CLOSED_REQUEST);
            return;
        }

        if (p->busy) {
            /*
             * The write filter still holds the submitted buffers in
             * r->out.  Resuming must pass a NULL chain: re-sending
             * out_head would queue the pending bytes a second time.
             */

            rc = ngx_http_output_filter(p->r, NULL);

            if (rc == NGX_ERROR) {
                ngx_live_flv_player_finish(p,
                                           NGX_HTTP_CLIENT_CLOSED_REQUEST);
                return;
            }

            if (rc == NGX_AGAIN) {
                break;
            }

            p->busy = 0;    /* NGX_OK: r->out drained */
            continue;
        }

        if (p->out_head == NULL) {
            break;    /* nothing new to submit */
        }

        out = p->out_head;

        rc = ngx_http_output_filter(p->r, out);

        if (rc == NGX_ERROR) {
            ngx_live_flv_player_finish(p, NGX_HTTP_CLIENT_CLOSED_REQUEST);
            return;
        }

        p->busy = 1;

        if (rc == NGX_AGAIN) {
            break;
        }

        p->busy = 0;        /* everything submitted went out */

        ngx_live_flv_player_drain(p, 0);

        if (p->out_head == out) {
            /*
             * Nothing was consumed (e.g. the terminal buf with
             * "chunked_transfer_encoding off", which no filter ever
             * clears): this queue can make no further progress.
             */
            break;
        }
    }

    if (rc == NGX_AGAIN) {
        clcf = ngx_http_get_module_loc_conf(p->r, ngx_http_core_module);

        if (!c->write->ready && !c->write->timer_set) {
            ngx_add_timer(c->write, clcf->send_timeout);
        }

        if (ngx_handle_write_event(c->write, clcf->send_lowat) != NGX_OK) {
            ngx_live_flv_player_finish(p, NGX_HTTP_INTERNAL_SERVER_ERROR);
            return;
        }

    } else if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    /* the stream ended and the trailer went out with the rest */

    if (p->ending && !p->busy) {
        ngx_live_flv_player_finish(p, NGX_HTTP_OK);
    }
}


/*
 * Pop consumed links from the head of the output chain.  A node is
 * complete when its tail link has been consumed: then the shared
 * tag reference is released and its bytes leave out_size.  With
 * "all" set, everything pending is released regardless of send
 * state.
 *
 * A consumed link has pos == last.  The not-yet-submitted chunked
 * trailer is an empty buf carrying last_buf, which the chunked
 * filter clears once it has framed it, so the "!last_buf" guard
 * keeps queued-but-unsent trailers in place while letting framed
 * ones go.
 *
 * last_progress only advances when links were actually consumed:
 * a stalled consumer shows no progress and trips the slow-consumer
 * cut in ngx_live_flv_player_flush().
 */

static void
ngx_live_flv_player_drain(ngx_live_flv_player_t *p, ngx_int_t all)
{
    ngx_chain_t             *cl;
    ngx_live_flv_out_node_t *node;

    while (p->out_head
           && (all
               || (p->out_head->buf->last == p->out_head->buf->pos
                   && !p->out_head->buf->last_buf)))
    {
        cl = p->out_head;
        p->out_head = cl->next;

        if (p->node_head && p->node_head->tail == cl) {
            node = p->node_head;
            p->node_head = node->next;

            if (p->node_head == NULL) {
                p->node_tail = NULL;
            }

            if (node->shared) {
                ngx_live_free_shared_chain(p->cacf, node->shared);
            }

            if (node->size > p->out_size) {
                p->out_size = 0;
            } else {
                p->out_size -= node->size;
            }

            p->last_progress = ngx_current_msec;
        }
    }

    if (all) {
        p->node_head = p->node_tail = NULL;
        p->out_head = NULL;
        p->out_size = 0;
    }
}


static void
ngx_live_flv_player_finish(ngx_live_flv_player_t *p, ngx_int_t status)
{
    ngx_http_request_t *r;

    if (p->closed) {
        return;
    }

    p->closed = 1;

    r = p->r;

    if (!p->detaching && p->stream) {
        p->detaching = 1;
        ngx_live_stream_unsubscribe(p->stream, &p->sub);
        p->stream = NULL;
    }

    if (r->connection->write->timer_set) {
        ngx_del_timer(r->connection->write);
    }

    ngx_http_finalize_request(r, status);
}


static void
ngx_live_flv_player_cleanup(void *data)
{
    ngx_live_flv_player_cleanup_t *pcln = data;
    ngx_live_flv_player_t         *p = pcln->player;

    /* release pending shared references */

    ngx_live_flv_player_drain(p, 1);

    if (!p->closed && !p->detaching && p->stream) {
        p->detaching = 1;
        ngx_live_stream_unsubscribe(p->stream, &p->sub);
        p->stream = NULL;
    }
}



static ngx_live_core_app_conf_t *
ngx_live_flv_find_application(ngx_str_t *app)
{
    ngx_uint_t                   n;
    ngx_live_core_app_conf_t   **cacfp;
    ngx_live_core_main_conf_t   *cmcf;

    cmcf = ngx_live_core_main_conf;

    if (cmcf == NULL) {
        return NULL;
    }

    cacfp = cmcf->applications.elts;

    for (n = 0; n < cmcf->applications.nelts; ++n, ++cacfp) {
        if ((*cacfp)->name.len == app->len
            && ngx_strncmp((*cacfp)->name.data, app->data, app->len) == 0)
        {
            return *cacfp;
        }
    }

    return NULL;
}


static ngx_int_t
ngx_live_flv_safe_name(ngx_str_t *name)
{
    size_t   i;
    u_char   c;

    if (name->len == 0 || name->len >= NGX_LIVE_MAX_NAME) {
        return 0;
    }

    for (i = 0; i < name->len; i++) {
        c = name->data[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-')
        {
            continue;
        }

        return 0;
    }

    return 1;
}


/*
 * Send "100 Continue" when the client asked for it.  Mirrors
 * ngx_http_test_expect() from ngx_http_request_body.c, which is
 * static and therefore unreachable for our own body reader.
 */

static ngx_int_t
ngx_live_flv_test_expect(ngx_http_request_t *r)
{
    ngx_int_t   n;
    ngx_str_t  *expect;

    if (r->expect_tested
        || r->headers_in.expect == NULL
        || r->http_version < NGX_HTTP_VERSION_11)
    {
        return NGX_OK;
    }

    r->expect_tested = 1;

    expect = &r->headers_in.expect->value;

    if (expect->len != sizeof("100-continue") - 1
        || ngx_strncasecmp(expect->data, (u_char *) "100-continue",
                           sizeof("100-continue") - 1) != 0)
    {
        return NGX_OK;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "live_flv: send 100 Continue");

    n = r->connection->send(r->connection,
                            (u_char *) "HTTP/1.1 100 Continue" CRLF CRLF,
                            sizeof("HTTP/1.1 100 Continue" CRLF CRLF) - 1);

    if (n == (ngx_int_t) sizeof("HTTP/1.1 100 Continue" CRLF CRLF) - 1) {
        return NGX_OK;
    }

    /* we assume that such small packet should be send successfully */

    r->connection->error = 1;

    return NGX_ERROR;
}


static void *
ngx_live_flv_create_loc_conf(ngx_conf_t *cf)
{
    ngx_live_flv_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_live_flv_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->flv = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_live_flv_merge_loc_conf(ngx_conf_t *cf, void *parent, void *conf)
{
    ngx_live_flv_loc_conf_t *prev = parent;
    ngx_live_flv_loc_conf_t *lcf = conf;

    ngx_conf_merge_value(lcf->flv, prev->flv, 0);

    if (lcf->flv == 0) {
        return NGX_CONF_OK;
    }

    if (ngx_live_core_main_conf == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "live_flv requires a \"live\" block "
                           "in the configuration");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_live_flv(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                      *rv;
    ngx_http_core_loc_conf_t  *clcf;

    rv = ngx_conf_set_flag_slot(cf, cmd, conf);
    if (rv != NGX_CONF_OK) {
        return rv;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_live_flv_handler;

    return NGX_CONF_OK;
}
