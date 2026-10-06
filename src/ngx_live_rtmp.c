
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <nginx.h>
#include "ngx_live.h"
#include "ngx_live_rtmp.h"


static void *ngx_live_rtmp_create_srv_conf(ngx_conf_t *cf);
static char *ngx_live_rtmp_merge_srv_conf(ngx_conf_t *cf, void *prev,
    void *conf);

static void ngx_live_rtmp_handshake_recv(ngx_event_t *rev);
static void ngx_live_rtmp_handshake_send(ngx_event_t *wev);
static void ngx_live_rtmp_handshake_done(ngx_live_rtmp_session_t *s);

static void ngx_live_rtmp_recv(ngx_event_t *rev);
static void ngx_live_rtmp_write(ngx_event_t *wev);
static void ngx_live_rtmp_cycle(ngx_live_rtmp_session_t *s);

static ngx_int_t ngx_live_rtmp_parse(ngx_live_rtmp_session_t *s,
    u_char *p, size_t len);
static ngx_int_t ngx_live_rtmp_receive(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_header_t *h, ngx_chain_t *in);

static ngx_chain_t *ngx_live_rtmp_in_alloc(ngx_live_rtmp_session_t *s);
ngx_int_t ngx_live_rtmp_in_append(ngx_live_rtmp_session_t *s,
    u_char *p, size_t n);
static void ngx_live_rtmp_in_free(ngx_live_rtmp_session_t *s,
    ngx_chain_t *in);

static ngx_live_rtmp_out_node_t *ngx_live_rtmp_node_alloc(
    ngx_live_rtmp_session_t *s);
void ngx_live_rtmp_node_free(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_out_node_t *node);
static ngx_live_rtmp_link_t *ngx_live_rtmp_link_alloc(
    ngx_live_rtmp_session_t *s);
static u_char *ngx_live_rtmp_body_alloc(ngx_live_rtmp_session_t *s);
static void ngx_live_rtmp_enqueue(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_out_node_t *node);
static ngx_uint_t ngx_live_rtmp_write_headers(
    ngx_live_rtmp_out_node_t *node, uint32_t csid, uint8_t type,
    uint32_t msid, uint32_t timestamp, uint32_t mlen, ngx_uint_t *pthsize);
static void ngx_live_rtmp_close_session_handler(ngx_event_t *ev);
void ngx_live_rtmp_close_connection(ngx_connection_t *c);
static u_char *ngx_live_rtmp_log_error(ngx_log_t *log, u_char *buf,
    size_t len);

static u_int ngx_live_rtmp_random(void);


#define NGX_LIVE_RTMP_IN_BUF_SIZE       4096
#define NGX_LIVE_RTMP_CBODY_SIZE        1024
#define NGX_LIVE_RTMP_HANDSHAKE_SIZE    3073    /* C0+C1+C2 == S0+S1+S2 */


static ngx_command_t  ngx_live_rtmp_commands[] = {

    { ngx_string("chunk_size"),
      NGX_LIVE_MAIN_CONF|NGX_LIVE_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_LIVE_SRV_CONF_OFFSET,
      offsetof(ngx_live_rtmp_srv_conf_t, chunk_size),
      NULL },

    { ngx_string("timeout"),
      NGX_LIVE_MAIN_CONF|NGX_LIVE_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_msec_slot,
      NGX_LIVE_SRV_CONF_OFFSET,
      offsetof(ngx_live_rtmp_srv_conf_t, timeout),
      NULL },

    { ngx_string("max_streams"),
      NGX_LIVE_MAIN_CONF|NGX_LIVE_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_LIVE_SRV_CONF_OFFSET,
      offsetof(ngx_live_rtmp_srv_conf_t, max_streams),
      NULL },

    { ngx_string("ack_window"),
      NGX_LIVE_MAIN_CONF|NGX_LIVE_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_LIVE_SRV_CONF_OFFSET,
      offsetof(ngx_live_rtmp_srv_conf_t, ack_window),
      NULL },

    { ngx_string("max_message"),
      NGX_LIVE_MAIN_CONF|NGX_LIVE_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_LIVE_SRV_CONF_OFFSET,
      offsetof(ngx_live_rtmp_srv_conf_t, max_message),
      NULL },

      ngx_null_command
};


static ngx_live_module_t  ngx_live_rtmp_module_ctx = {
    NULL,                               /* preconfiguration */
    NULL,                               /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    ngx_live_rtmp_create_srv_conf,      /* create server configuration */
    ngx_live_rtmp_merge_srv_conf,       /* merge server configuration */

    NULL,                               /* create app configuration */
    NULL                                /* merge app configuration */
};


ngx_module_t  ngx_live_rtmp_module = {
    NGX_MODULE_V1,
    &ngx_live_rtmp_module_ctx,          /* module context */
    ngx_live_rtmp_commands,             /* module directives */
    NGX_LIVE_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_live_rtmp_create_srv_conf(ngx_conf_t *cf)
{
    ngx_live_rtmp_srv_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_live_rtmp_srv_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->chunk_size = NGX_CONF_UNSET;
    conf->timeout = NGX_CONF_UNSET_MSEC;
    conf->max_streams = NGX_CONF_UNSET_UINT;
    conf->ack_window = NGX_CONF_UNSET_UINT;
    conf->max_message = NGX_CONF_UNSET_SIZE;

    return conf;
}


static char *
ngx_live_rtmp_merge_srv_conf(ngx_conf_t *cf, void *parent, void *conf)
{
    ngx_live_rtmp_srv_conf_t *prev = parent;
    ngx_live_rtmp_srv_conf_t *lrsconf = conf;

    ngx_conf_merge_value(lrsconf->chunk_size, prev->chunk_size, 65535);
    ngx_conf_merge_msec_value(lrsconf->timeout, prev->timeout, 60000);
    ngx_conf_merge_uint_value(lrsconf->max_streams, prev->max_streams, 64);
    ngx_conf_merge_uint_value(lrsconf->ack_window, prev->ack_window, 5000000);
    ngx_conf_merge_size_value(lrsconf->max_message, prev->max_message,
                              4 * 1024 * 1024);

    if (lrsconf->chunk_size < 1024
        || lrsconf->chunk_size > NGX_LIVE_RTMP_MAX_CHUNK_SIZE)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "chunk_size must be between 1024 and %d",
                           NGX_LIVE_RTMP_MAX_CHUNK_SIZE);
        return NGX_CONF_ERROR;
    }

    if (lrsconf->max_streams < 16) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "max_streams must be at least 16");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


/*
 * Connection setup
 */

void
ngx_live_rtmp_init_connection(ngx_connection_t *c)
{
    ngx_live_rtmp_session_t  *s;
    ngx_live_rtmp_addr_conf_t *addr_conf;

    addr_conf = c->listening->servers;

    s = ngx_pcalloc(c->pool, sizeof(ngx_live_rtmp_session_t));
    if (s == NULL) {
        ngx_live_rtmp_close_connection(c);
        return;
    }

    s->connection = c;
    s->ctx = addr_conf->ctx;

    s->conf = s->ctx->srv_conf[ngx_live_rtmp_module.ctx_index];

    c->data = s;

    c->log->connection = c->number;
    c->log->handler = ngx_live_rtmp_log_error;
    c->log->data = &c->addr_text;
    c->log->action = "handshaking";

    s->in_streams = ngx_pcalloc(c->pool,
                                sizeof(ngx_live_rtmp_istream_t)
                                * s->conf->max_streams);
    if (s->in_streams == NULL) {
        ngx_live_rtmp_close_connection(c);
        return;
    }

    s->in_chunk_size = NGX_LIVE_RTMP_DEFAULT_CHUNK_SIZE;
    s->pstate = NGX_LIVE_RTMP_ST_BASIC;
    s->basic_need = 1;
    s->last_progress = ngx_current_msec;

    s->hs_in = ngx_create_temp_buf(c->pool, NGX_LIVE_RTMP_HANDSHAKE_SIZE);
    s->hs_out = ngx_create_temp_buf(c->pool, NGX_LIVE_RTMP_HANDSHAKE_SIZE);

    if (s->hs_in == NULL || s->hs_out == NULL) {
        ngx_live_rtmp_close_connection(c);
        return;
    }

    s->hs_stage = NGX_LIVE_RTMP_HS_C0C1;

    ngx_log_error(NGX_LOG_INFO, c->log, 0,
                  "live rtmp: client connected '%V'", &c->addr_text);

    c->read->handler = ngx_live_rtmp_handshake_recv;
    c->write->handler = ngx_live_rtmp_handshake_send;

    ngx_add_timer(c->read, s->conf->timeout);

    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        ngx_live_rtmp_close_connection(c);
    }
}


static u_char *
ngx_live_rtmp_log_error(ngx_log_t *log, u_char *buf, size_t len)
{
    u_char  *p;
    ngx_str_t *addr;

    p = buf;

    if (log->action) {
        p = ngx_snprintf(buf, len, " while %s", log->action);
        len -= p - buf;
        buf = p;
    }

    addr = log->data;

    p = ngx_snprintf(buf, len, ", client: %V", addr);

    return p;
}


/*
 * Simple handshake:
 *
 *   C0 (1 byte version) + C1 (1536 bytes)  ->
 *                                       <-  S0 (0x03) + S1 + S2(echo of C1)
 *   C2 (echo of S1, unchecked)           ->
 *
 * C0 version and C2 content are accepted as-is.
 */

static void
ngx_live_rtmp_handshake_recv(ngx_event_t *rev)
{
    ngx_int_t                 n;
    ngx_buf_t                *b;
    ngx_connection_t         *c;
    ngx_live_rtmp_session_t  *s;

    c = rev->data;
    s = c->data;
    b = s->hs_in;

    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "live rtmp: handshake timed out");
        ngx_live_rtmp_finalize_session(s);
        return;
    }

    for ( ;; ) {

        n = c->recv(c, b->last, b->end - b->last);

        if (n == NGX_ERROR || n == 0) {
            ngx_live_rtmp_finalize_session(s);
            return;
        }

        if (n == NGX_AGAIN) {
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_live_rtmp_finalize_session(s);
            }
            return;
        }

        b->last += n;

        if (s->hs_stage == NGX_LIVE_RTMP_HS_C0C1
            && b->last - b->start >= 1537)
        {
            /* C0C1 complete: build and send S0S1S2 */

            u_char  *out;
            u_int    i;
            time_t   now;

            s->hs_stage = NGX_LIVE_RTMP_HS_C2;

            out = s->hs_out->start;

            out[0] = NGX_LIVE_RTMP_VERSION;             /* S0 */

            now = ngx_time();

            out[1] = (u_char) (now >> 24);              /* S1: time */
            out[2] = (u_char) (now >> 16);
            out[3] = (u_char) (now >> 8);
            out[4] = (u_char) now;

            ngx_memzero(&out[5], 4);                    /* zero */

            for (i = 9; i < 1537; i++) {                /* random */
                out[i] = (u_char) ngx_live_rtmp_random();
            }

            ngx_memcpy(&out[1537], &b->start[1], 1536); /* S2 = echo C1 */

            s->hs_out->last = s->hs_out->end;

            ngx_live_rtmp_handshake_send(c->write);

            if (c->destroyed) {
                return;
            }
        }

        if (s->hs_stage == NGX_LIVE_RTMP_HS_C2 && b->last == b->end) {
            ngx_live_rtmp_handshake_done(s);
            return;
        }
    }
}


static void
ngx_live_rtmp_handshake_send(ngx_event_t *wev)
{
    ngx_int_t                 n;
    ngx_buf_t                *b;
    ngx_connection_t         *c;
    ngx_live_rtmp_session_t  *s;

    c = wev->data;
    s = c->data;

    if (c->destroyed) {
        return;
    }

    if (wev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "live rtmp: handshake send timed out");
        ngx_live_rtmp_finalize_session(s);
        return;
    }

    b = s->hs_out;

    while (b->pos < b->last) {

        n = c->send(c, b->pos, b->last - b->pos);

        if (n == NGX_AGAIN || n == 0) {
            ngx_add_timer(c->write, s->conf->timeout);

            if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
                ngx_live_rtmp_finalize_session(s);
            }

            return;
        }

        if (n < 0) {
            ngx_live_rtmp_finalize_session(s);
            return;
        }

        b->pos += n;
    }

    if (wev->active) {
        ngx_del_event(wev, NGX_WRITE_EVENT, 0);
    }
}


static void
ngx_live_rtmp_handshake_done(ngx_live_rtmp_session_t *s)
{
    ngx_connection_t  *c;

    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "live rtmp: handshake done");

    s->hs_stage = NGX_LIVE_RTMP_HS_DONE;

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    ngx_live_rtmp_cycle(s);
}


static void
ngx_live_rtmp_cycle(ngx_live_rtmp_session_t *s)
{
    ngx_connection_t  *c;

    c = s->connection;

    c->log->action = "processing RTMP";

    c->read->handler = ngx_live_rtmp_recv;
    c->write->handler = ngx_live_rtmp_write;

    /* drain whatever arrived together with C2 */

    ngx_live_rtmp_recv(c->read);
}


/*
 * Receive loop: read raw bytes and feed the chunk parser.
 */

static void
ngx_live_rtmp_recv(ngx_event_t *rev)
{
    u_char                   buf[16384];
    ngx_int_t                n;
    ngx_connection_t        *c;
    ngx_live_rtmp_session_t *s;

    c = rev->data;
    s = c->data;

    if (c->destroyed) {
        return;
    }

    if (rev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "live rtmp: %s timed out",
                      s->pub_stream ? "publish" : "connection");
        c->timedout = 1;
        ngx_live_rtmp_finalize_session(s);
        return;
    }

    for ( ;; ) {

        n = c->recv(c, buf, sizeof(buf));

        if (n == NGX_ERROR || n == 0) {
            ngx_live_rtmp_finalize_session(s);
            return;
        }

        if (n == NGX_AGAIN) {
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_live_rtmp_finalize_session(s);
            }
            return;
        }

        if (s->pub_stream && s->conf->timeout) {
            ngx_add_timer(c->read, s->conf->timeout);
        }

        s->in_bytes += n;

        if (s->in_bytes >= 0xf0000000) {
            /* keep the 32-bit ack sequence from wrapping too early */
            s->in_bytes = 0;
            s->in_last_ack = 0;
        }

        if (s->in_ack_size
            && s->in_bytes - s->in_last_ack >= s->in_ack_size)
        {
            s->in_last_ack = s->in_bytes;

            if (ngx_live_rtmp_send_ack(s, s->in_bytes) != NGX_OK) {
                ngx_live_rtmp_finalize_session(s);
                return;
            }
        }

        if (ngx_live_rtmp_parse(s, buf, (size_t) n) != NGX_OK) {
            ngx_live_rtmp_finalize_session(s);
            return;
        }
    }
}


/*
 * Chunk parser state machine.  Consumes arbitrary slices of the
 * byte stream; state is kept in the session.  See the RTMP
 * specification, section 5.3 (Chunk Stream).
 */

static ngx_int_t
ngx_live_rtmp_parse(ngx_live_rtmp_session_t *s, u_char *p, size_t len)
{
    size_t                    take;
    ngx_uint_t                fmt, need;
    uint32_t                  csid, ts, mlen, msid;
    ngx_live_rtmp_header_t    hdr;
    ngx_live_rtmp_istream_t  *st;

    while (len) {

        switch (s->pstate) {

        case NGX_LIVE_RTMP_ST_BASIC:

            while (s->basic_got < s->basic_need && len) {
                s->basic[s->basic_got++] = *p++;
                len--;

                if (s->basic_got == 1) {
                    csid = s->basic[0] & 0x3f;

                    if (csid == 0) {
                        s->basic_need = 2;

                    } else if (csid == 1) {
                        s->basic_need = 3;
                    }
                }
            }

            if (s->basic_got < s->basic_need) {
                return NGX_OK;    /* need more bytes */
            }

            fmt = s->basic[0] >> 6;
            csid = s->basic[0] & 0x3f;

            if (csid == 0) {
                csid = 64 + s->basic[1];

            } else if (csid == 1) {
                csid = 64 + s->basic[1] + 256 * s->basic[2];
            }

            s->basic_got = 0;
            s->basic_need = 1;

            if (csid >= s->conf->max_streams) {
                ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                              "live rtmp: chunk stream id %uD too big", csid);
                return NGX_ERROR;
            }

            s->pend_fmt = fmt;
            s->pend_csid = csid;

            st = &s->in_streams[csid];

            if (fmt == 3) {
                /* continuation or repeat: no message header */

                if (st->ext) {
                    /* librtmp-style clients repeat the extended
                     * timestamp on every fmt3 chunk */
                    s->mhdr_got = 0;
                    s->mhdr_need = 4;
                    s->pstate = NGX_LIVE_RTMP_ST_EXTTS;

                } else {
                    s->frag_len = 0;
                    s->pstate = NGX_LIVE_RTMP_ST_PAYLOAD;
                }

                break;
            }

            s->mhdr_need = (fmt == 0) ? 11 : (fmt == 1 ? 7 : 3);
            s->mhdr_got = 0;
            s->pstate = NGX_LIVE_RTMP_ST_MSGHDR;
            break;

        case NGX_LIVE_RTMP_ST_MSGHDR:

            while (s->mhdr_got < s->mhdr_need && len) {
                s->mhdr[s->mhdr_got++] = *p++;
                len--;
            }

            if (s->mhdr_got < s->mhdr_need) {
                return NGX_OK;
            }

            fmt = s->pend_fmt;
            st = &s->in_streams[s->pend_csid];

            ts = ((uint32_t) s->mhdr[0] << 16)
                 | ((uint32_t) s->mhdr[1] << 8) | s->mhdr[2];

            mlen = st->hdr.mlen;
            msid = st->hdr.msid;

            if (fmt <= 1) {
                mlen = ((uint32_t) s->mhdr[3] << 16)
                       | ((uint32_t) s->mhdr[4] << 8) | s->mhdr[5];

                if (fmt == 0) {
                    /* message stream id is little-endian */
                    msid = (uint32_t) s->mhdr[7]
                           | ((uint32_t) s->mhdr[8] << 8)
                           | ((uint32_t) s->mhdr[9] << 16)
                           | ((uint32_t) s->mhdr[10] << 24);
                }
            }

            s->pend_ext = (ts == 0x00ffffff);

            if (st->len == 0) {
                /* new message on this chunk stream */

                if (mlen > s->conf->max_message) {
                    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                                  "live rtmp: too big message %uD", mlen);
                    return NGX_ERROR;
                }

                if (fmt == 0) {
                    st->hdr.timestamp = ts;
                    st->dtime = 0;

                } else {
                    st->dtime = ts;
                }

                if (fmt <= 1) {
                    st->hdr.mlen = mlen;
                    st->hdr.type = s->mhdr[6];
                }

                if (fmt == 0) {
                    st->hdr.msid = msid;
                }

                /*
                 * Whether following fmt3 chunks carry the extended
                 * timestamp repeat is decided by this header; for a
                 * fmt3-started message the previous value persists.
                 */
                st->ext = s->pend_ext;
            }

            if (s->pend_ext) {
                s->mhdr_got = 0;
                s->mhdr_need = 4;
                s->pstate = NGX_LIVE_RTMP_ST_EXTTS;

            } else {
                s->frag_len = 0;
                s->pstate = NGX_LIVE_RTMP_ST_PAYLOAD;
            }

            break;

        case NGX_LIVE_RTMP_ST_EXTTS:

            while (s->mhdr_got < s->mhdr_need && len) {
                s->mhdr[s->mhdr_got++] = *p++;
                len--;
            }

            if (s->mhdr_got < s->mhdr_need) {
                return NGX_OK;
            }

            st = &s->in_streams[s->pend_csid];

            if (st->len == 0 && s->pend_fmt <= 2) {
                /* the extended value replaces the 0xffffff field */

                ts = ((uint32_t) s->mhdr[0] << 24)
                     | ((uint32_t) s->mhdr[1] << 16)
                     | ((uint32_t) s->mhdr[2] << 8) | s->mhdr[3];

                if (s->pend_fmt == 0) {
                    st->hdr.timestamp = ts;

                } else {
                    st->dtime = ts;
                }
            }

            s->frag_len = 0;
            s->pstate = NGX_LIVE_RTMP_ST_PAYLOAD;
            break;

        case NGX_LIVE_RTMP_ST_PAYLOAD:

            st = &s->in_streams[s->pend_csid];

            if (s->frag_len == 0) {
                need = st->hdr.mlen - st->len;

                if (need > s->in_chunk_size) {
                    need = s->in_chunk_size;
                }

                s->frag_len = need;
            }

            take = s->frag_len - s->frag_pos;
            take = ngx_min(take, len);

            if (take
                && ngx_live_rtmp_in_append(s, p, take) != NGX_OK)
            {
                return NGX_ERROR;
            }

            st->len += take;
            s->frag_pos += take;
            p += take;
            len -= take;

            if (s->frag_pos < s->frag_len) {
                return NGX_OK;    /* rest of the chunk arrives later */
            }

            /* chunk complete */

            s->frag_pos = 0;
            s->frag_len = 0;

            if (st->len < st->hdr.mlen) {
                /* more chunks of this message follow */
                s->pstate = NGX_LIVE_RTMP_ST_BASIC;
                break;
            }

            /* message complete */

            st->hdr.timestamp += st->dtime;
            st->hdr.csid = s->pend_csid;

            hdr = st->hdr;

            if (ngx_live_rtmp_receive(s, &hdr, st->in) != NGX_OK) {
                return NGX_ERROR;
            }

            if (st->in) {
                ngx_live_rtmp_in_free(s, st->in);
            }

            st->in = NULL;
            st->in_tail = NULL;
            st->len = 0;

            s->pstate = NGX_LIVE_RTMP_ST_BASIC;
            break;

        default:
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


/*
 * Dispatch one complete message.
 */

static ngx_int_t
ngx_live_rtmp_receive(ngx_live_rtmp_session_t *s, ngx_live_rtmp_header_t *h,
    ngx_chain_t *in)
{
    uint32_t       val;
    u_char         b[8];
    ngx_log_t     *log;

    log = s->connection->log;

    ngx_log_debug5(NGX_LOG_DEBUG_HTTP, log, 0,
                   "live rtmp: recv type=%ui csid=%uD ts=%uD "
                   "mlen=%uD msid=%uD",
                   (ngx_uint_t) h->type, h->csid, h->timestamp,
                   h->mlen, h->msid);

    switch (h->type) {

    case NGX_LIVE_RTMP_MSG_CHUNK_SIZE:

        if (ngx_live_chain_read(in, b, 4) < 4) {
            return NGX_ERROR;
        }

        val = ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16)
              | ((uint32_t) b[2] << 8) | b[3];

        if (val == 0 || (val & 0x80000000)) {
            ngx_log_error(NGX_LOG_INFO, log, 0,
                          "live rtmp: invalid chunk size %uD", val);
            return NGX_ERROR;
        }

        if (val > NGX_LIVE_RTMP_MAX_CHUNK_SIZE) {
            val = NGX_LIVE_RTMP_MAX_CHUNK_SIZE;
        }

        s->in_chunk_size = val;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                       "live rtmp: in chunk size set to %uD", val);
        return NGX_OK;

    case NGX_LIVE_RTMP_MSG_ABORT:

        if (ngx_live_chain_read(in, b, 4) == 4) {
            val = ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16)
                  | ((uint32_t) b[2] << 8) | b[3];

            if (val < s->conf->max_streams) {
                ngx_live_rtmp_istream_t  *bst;

                bst = &s->in_streams[val];

                if (bst->in) {
                    ngx_live_rtmp_in_free(s, bst->in);
                }

                bst->in = NULL;
                bst->in_tail = NULL;
                bst->len = 0;
            }
        }

        return NGX_OK;

    case NGX_LIVE_RTMP_MSG_ACK:
        return NGX_OK;

    case NGX_LIVE_RTMP_MSG_ACK_SIZE:

        if (ngx_live_chain_read(in, b, 4) == 4) {
            s->in_ack_size = ((uint32_t) b[0] << 24)
                             | ((uint32_t) b[1] << 16)
                             | ((uint32_t) b[2] << 8) | b[3];
        }

        return NGX_OK;

    case NGX_LIVE_RTMP_MSG_BANDWIDTH:
        return NGX_OK;

    case NGX_LIVE_RTMP_MSG_USER:

        if (ngx_live_chain_read(in, b, 2) < 2) {
            return NGX_OK;
        }

        val = ((uint32_t) b[0] << 8) | b[1];

        if (val == NGX_LIVE_RTMP_USER_PING_REQUEST) {
            if (ngx_live_chain_read(in, &b[2], 4) == 4) {
                return ngx_live_rtmp_send_user_control(s,
                          NGX_LIVE_RTMP_USER_PING_RESPONSE,
                          ((uint32_t) b[2] << 24) | ((uint32_t) b[3] << 16)
                          | ((uint32_t) b[4] << 8) | b[5]);
            }
        }

        return NGX_OK;

    case NGX_LIVE_RTMP_MSG_AUDIO:
    case NGX_LIVE_RTMP_MSG_VIDEO:
    case NGX_LIVE_RTMP_MSG_AMF_META:
        return ngx_live_rtmp_media(s, h, in);

    case NGX_LIVE_RTMP_MSG_AMF_CMD:
        return ngx_live_rtmp_command(s, h, in);

    case NGX_LIVE_RTMP_MSG_AMF3_CMD:
    case NGX_LIVE_RTMP_MSG_AMF3_META:
    case NGX_LIVE_RTMP_MSG_AMF3_SHARED:
    case NGX_LIVE_RTMP_MSG_AMF_SHARED:
    case NGX_LIVE_RTMP_MSG_AGGREGATE:
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                       "live rtmp: ignoring message type=%ui",
                       (ngx_uint_t) h->type);
        return NGX_OK;

    default:
        return NGX_OK;
    }
}


/*
 * Input reassembly buffers
 */

static ngx_chain_t *
ngx_live_rtmp_in_alloc(ngx_live_rtmp_session_t *s)
{
    u_char       *p;
    ngx_chain_t  *cl;
    ngx_buf_t    *b;

    if (s->in_free) {
        cl = s->in_free;
        s->in_free = cl->next;

    } else {
        p = ngx_palloc(s->connection->pool,
                       sizeof(ngx_chain_t) + sizeof(ngx_buf_t)
                       + NGX_LIVE_RTMP_IN_BUF_SIZE);
        if (p == NULL) {
            return NULL;
        }

        cl = (ngx_chain_t *) p;
        p += sizeof(ngx_chain_t);
        cl->buf = (ngx_buf_t *) p;
        p += sizeof(ngx_buf_t);
        cl->buf->start = p;
        cl->buf->end = p + NGX_LIVE_RTMP_IN_BUF_SIZE;
    }

    cl->next = NULL;

    b = cl->buf;
    b->pos = b->last = b->start;
    b->memory = 1;

    return cl;
}


ngx_int_t
ngx_live_rtmp_in_append(ngx_live_rtmp_session_t *s, u_char *p, size_t n)
{
    u_char                   *dst;
    size_t                    take;
    ngx_buf_t                *b;
    ngx_live_rtmp_istream_t  *st;

    st = &s->in_streams[s->pend_csid];

    while (n) {

        if (st->in_tail == NULL) {
            st->in_tail = ngx_live_rtmp_in_alloc(s);

            if (st->in_tail == NULL) {
                return NGX_ERROR;
            }

            if (st->in == NULL) {
                st->in = st->in_tail;
            }

        } else if (st->in_tail->buf->last == st->in_tail->buf->end) {
            ngx_chain_t  *cl;

            cl = ngx_live_rtmp_in_alloc(s);
            if (cl == NULL) {
                return NGX_ERROR;
            }

            st->in_tail->next = cl;
            st->in_tail = cl;
        }

        b = st->in_tail->buf;
        dst = b->last;

        take = (size_t) (b->end - b->last);
        take = ngx_min(take, n);

        b->last = ngx_cpymem(dst, p, take);
        p += take;
        n -= take;
    }

    return NGX_OK;
}


static void
ngx_live_rtmp_in_free(ngx_live_rtmp_session_t *s, ngx_chain_t *in)
{
    ngx_chain_t  *cl;

    for (cl = in; ; cl = cl->next) {
        if (cl->next == NULL) {
            cl->next = s->in_free;
            s->in_free = in;
            return;
        }
    }
}


/*
 * Output queue
 */

static ngx_live_rtmp_out_node_t *
ngx_live_rtmp_node_alloc(ngx_live_rtmp_session_t *s)
{
    ngx_live_rtmp_out_node_t  *node;

    if (s->free_nodes) {
        node = s->free_nodes;
        s->free_nodes = (ngx_live_rtmp_out_node_t *) node->next;

    } else {
        node = ngx_palloc(s->connection->pool,
                          sizeof(ngx_live_rtmp_out_node_t));
    }

    if (node == NULL) {
        return NULL;
    }

    ngx_memzero(node, sizeof(ngx_live_rtmp_out_node_t));

    return node;
}


void
ngx_live_rtmp_node_free(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_out_node_t *node)
{
    ngx_chain_t          *cl, *next;
    ngx_live_rtmp_link_t *link;

    for (cl = node->chain; cl; cl = next) {
        next = cl->next;
        link = (ngx_live_rtmp_link_t *) cl;
        cl->next = (ngx_chain_t *) s->free_links;
        s->free_links = link;
    }

    if (node->shared && s->cacf) {
        ngx_live_free_shared_chain(s->cacf, node->shared);
    }

    if (node->body) {
        *(u_char **) node->body = s->free_bodies;
        s->free_bodies = node->body;
    }

    node->chain = NULL;
    node->tail = NULL;
    node->shared = NULL;
    node->body = NULL;

    node->next = (ngx_live_rtmp_out_node_t *) s->free_nodes;
    s->free_nodes = node;
}


static ngx_live_rtmp_link_t *
ngx_live_rtmp_link_alloc(ngx_live_rtmp_session_t *s)
{
    ngx_live_rtmp_link_t *link;

    if (s->free_links) {
        link = s->free_links;
        s->free_links = (ngx_live_rtmp_link_t *) link->cl.next;

    } else {
        link = ngx_palloc(s->connection->pool, sizeof(ngx_live_rtmp_link_t));
        if (link == NULL) {
            return NULL;
        }
    }

    link->cl.next = NULL;
    link->cl.buf = &link->buf;
    ngx_memzero(&link->buf, sizeof(ngx_buf_t));
    link->buf.memory = 1;

    return link;
}


static u_char *
ngx_live_rtmp_body_alloc(ngx_live_rtmp_session_t *s)
{
    u_char  *b;

    if (s->free_bodies) {
        b = s->free_bodies;
        s->free_bodies = *(u_char **) b;
        return b;
    }

    return ngx_palloc(s->connection->pool, NGX_LIVE_RTMP_CBODY_SIZE);
}


static void
ngx_live_rtmp_enqueue(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_out_node_t *node)
{
    if (s->out_tail) {
        s->out_tail->next = node;
        s->out_tail = node;

    } else {
        s->out_head = node;
        s->out_tail = node;
    }

    s->out_size += node->size;
}


/*
 * Build the chunk headers for one outgoing message into node->h.
 *
 *   h[0..hsize0)    fmt0 header of the first chunk
 *   h[hsize0..+thsize)  fmt3 template, shared by every continuation
 *                       chunk (they are all identical)
 *
 * Returns hsize0, or 0 on error.
 */

static ngx_uint_t
ngx_live_rtmp_write_headers(ngx_live_rtmp_out_node_t *node, uint32_t csid,
    uint8_t type, uint32_t msid, uint32_t timestamp, uint32_t mlen,
    ngx_uint_t *pthsize)
{
    u_char     *p;
    ngx_uint_t  ext, hsize, thsize;

    ext = (timestamp >= 0x00ffffff);

    p = node->h;

    /* basic header, fmt0; our csids are all < 64 */

    *p++ = (u_char) csid;

    if (ext) {
        *p++ = 0xff;
        *p++ = 0xff;
        *p++ = 0xff;

    } else {
        *p++ = (u_char) (timestamp >> 16);
        *p++ = (u_char) (timestamp >> 8);
        *p++ = (u_char) timestamp;
    }

    *p++ = (u_char) (mlen >> 16);
    *p++ = (u_char) (mlen >> 8);
    *p++ = (u_char) mlen;
    *p++ = type;

    *p++ = (u_char) msid;
    *p++ = (u_char) (msid >> 8);
    *p++ = (u_char) (msid >> 16);
    *p++ = (u_char) (msid >> 24);

    if (ext) {
        *p++ = (u_char) (timestamp >> 24);
        *p++ = (u_char) (timestamp >> 16);
        *p++ = (u_char) (timestamp >> 8);
        *p++ = (u_char) timestamp;
    }

    hsize = p - node->h;

    /* fmt3 template for continuation chunks */

    *p = (u_char) (0xc0 | csid);
    thsize = 1;

    if (ext) {
        /* repeated extended timestamp, librtmp-style */
        p[1] = (u_char) (timestamp >> 24);
        p[2] = (u_char) (timestamp >> 16);
        p[3] = (u_char) (timestamp >> 8);
        p[4] = (u_char) timestamp;
        thsize = 5;
    }

    *pthsize = thsize;

    return hsize;
}


/*
 * Queue one message with a contiguous body (protocol and command
 * messages; small enough to fit a single chunk).
 */

ngx_int_t
ngx_live_rtmp_send(ngx_live_rtmp_session_t *s, uint8_t type, uint32_t csid,
    uint32_t msid, uint32_t timestamp, const u_char *body, size_t len)
{
    u_char                   *b;
    ngx_uint_t                hsize, thsize;
    ngx_live_rtmp_link_t     *hl, *bl;
    ngx_live_rtmp_out_node_t *node;

    if (len > NGX_LIVE_RTMP_CBODY_SIZE) {
        return NGX_ERROR;
    }

    node = ngx_live_rtmp_node_alloc(s);
    hl = ngx_live_rtmp_link_alloc(s);
    bl = ngx_live_rtmp_link_alloc(s);

    b = ngx_live_rtmp_body_alloc(s);

    if (node == NULL || hl == NULL || bl == NULL || b == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(b, body, len);

    hsize = ngx_live_rtmp_write_headers(node, csid, type, msid,
                                        timestamp, len, &thsize);
    if (hsize == 0) {
        return NGX_ERROR;
    }

    hl->buf.pos = node->h;
    hl->buf.last = node->h + hsize;

    bl->buf.pos = b;
    bl->buf.last = b + len;

    hl->cl.next = &bl->cl;

    node->chain = &hl->cl;
    node->tail = &bl->cl;
    node->body = b;
    node->size = len;

    ngx_live_rtmp_enqueue(s, node);

    ngx_live_rtmp_flush(s);

    return NGX_OK;
}


/*
 * Queue one cached stream packet.  The FLV tag header (11 bytes at
 * the head of the first shared buffer) is skipped; the tag body is
 * the RTMP message payload.  Chunk boundaries fall on chunk_size or
 * buffer boundaries, whichever comes first; a slice of a shared
 * buffer may end mid-buffer, which is why chunk headers live in
 * their own links.
 */

ngx_int_t
ngx_live_rtmp_send_packet(ngx_live_rtmp_session_t *s, uint32_t csid,
    uint32_t msid, ngx_live_packet_t *pkt)
{
    u_char                   *sp, *se;
    size_t                   take;
    ngx_uint_t               hsize, thsize, chunk_left, first;
    ngx_chain_t             *cl, **ll, *last;
    ngx_live_rtmp_link_t    *link;
    ngx_live_rtmp_out_node_t *node;

    node = ngx_live_rtmp_node_alloc(s);
    if (node == NULL) {
        return NGX_ERROR;
    }

    hsize = ngx_live_rtmp_write_headers(node, csid,
                                        (u_char) pkt->codec_type, msid,
                                        pkt->dts, pkt->size, &thsize);
    if (hsize == 0) {
        return NGX_ERROR;
    }

    link = ngx_live_rtmp_link_alloc(s);
    if (link == NULL) {
        return NGX_ERROR;
    }

    link->buf.pos = node->h;
    link->buf.last = node->h + hsize;

    ll = &link->cl.next;
    node->chain = &link->cl;

    first = 1;
    chunk_left = s->conf->chunk_size;

    for (cl = pkt->chain; cl; cl = cl->next) {

        sp = cl->buf->pos;
        se = cl->buf->last;

        if (first) {
            /* skip the 11-byte FLV tag header */
            if (se - sp < 11) {
                return NGX_ERROR;
            }

            sp += 11;
            first = 0;
        }

        while (sp < se) {

            if (chunk_left == 0) {
                /* start a new chunk with the fmt3 template */

                link = ngx_live_rtmp_link_alloc(s);
                if (link == NULL) {
                    return NGX_ERROR;
                }

                link->buf.pos = node->h + hsize;
                link->buf.last = node->h + hsize + thsize;

                *ll = &link->cl;
                ll = &link->cl.next;

                chunk_left = s->conf->chunk_size;
            }

            take = (size_t) (se - sp);
            take = ngx_min(take, chunk_left);

            link = ngx_live_rtmp_link_alloc(s);
            if (link == NULL) {
                return NGX_ERROR;
            }

            link->buf.pos = sp;
            link->buf.last = sp + take;
            link->buf.start = cl->buf->start;
            link->buf.end = cl->buf->end;

            *ll = &link->cl;
            ll = &link->cl.next;

            sp += take;
            chunk_left -= take;
        }
    }

    *ll = NULL;

    /* the last link marks the end of the message */

    last = node->chain;

    while (last->next) {
        last = last->next;
    }

    node->tail = last;

    if (pkt->size) {
        ngx_live_acquire_shared_chain(pkt->chain);
        node->shared = pkt->chain;
    }

    node->size = pkt->size;

    ngx_live_rtmp_enqueue(s, node);

    ngx_live_rtmp_flush(s);

    return NGX_OK;
}


void
ngx_live_rtmp_flush(ngx_live_rtmp_session_t *s)
{
    ngx_int_t                 n;
    ngx_event_t              *wev;
    ngx_connection_t         *c;
    ngx_live_rtmp_out_node_t *node;

    c = s->connection;
    wev = c->write;

    if (c->destroyed) {
        return;
    }

    if (wev->timedout) {
        ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT,
                      "live rtmp: send timed out");
        c->timedout = 1;
        ngx_live_rtmp_finalize_session(s);
        return;
    }

    while (s->out_head) {

        if (s->out_link == NULL) {
            s->out_link = s->out_head->chain;
            s->out_pos = s->out_link->buf->pos;
        }

        n = c->send(c, s->out_pos, s->out_link->buf->last - s->out_pos);

        if (n == NGX_AGAIN || n == 0) {
            ngx_add_timer(wev, s->conf->timeout);

            if (ngx_handle_write_event(wev, 0) != NGX_OK) {
                ngx_live_rtmp_finalize_session(s);
            }

            return;
        }

        if (n < 0) {
            ngx_live_rtmp_finalize_session(s);
            return;
        }

        s->out_pos += n;

        if (s->out_pos < s->out_link->buf->last) {
            continue;
        }

        if (s->out_link == s->out_head->tail) {
            /* message fully sent */

            node = s->out_head;
            s->out_head = node->next;

            if (s->out_head == NULL) {
                s->out_tail = NULL;
            }

            if (node->size > s->out_size) {
                s->out_size = 0;
            } else {
                s->out_size -= node->size;
            }

            s->last_progress = ngx_current_msec;

            ngx_live_rtmp_node_free(s, node);

            s->out_link = NULL;

        } else {
            s->out_link = s->out_link->next;
            s->out_pos = s->out_link->buf->pos;
        }
    }

    /* queue drained */

    if (wev->timer_set) {
        ngx_del_timer(wev);
    }

    if (wev->active) {
        ngx_del_event(wev, NGX_WRITE_EVENT, 0);
    }
}


static void
ngx_live_rtmp_write(ngx_event_t *wev)
{
    ngx_connection_t         *c;
    ngx_live_rtmp_session_t  *s;

    c = wev->data;
    s = c->data;

    if (c->destroyed) {
        return;
    }

    if (wev->timer_set) {
        ngx_del_timer(wev);
    }

    ngx_live_rtmp_flush(s);
}


/*
 * Protocol helpers
 */

ngx_int_t
ngx_live_rtmp_send_ack(ngx_live_rtmp_session_t *s, uint32_t seq)
{
    u_char  b[4];

    b[0] = (u_char) (seq >> 24);
    b[1] = (u_char) (seq >> 16);
    b[2] = (u_char) (seq >> 8);
    b[3] = (u_char) seq;

    return ngx_live_rtmp_send(s, NGX_LIVE_RTMP_MSG_ACK,
                              NGX_LIVE_RTMP_CSID_PROTOCOL, 0, 0, b, 4);
}


ngx_int_t
ngx_live_rtmp_send_window_ack_size(ngx_live_rtmp_session_t *s, uint32_t size)
{
    u_char  b[4];

    b[0] = (u_char) (size >> 24);
    b[1] = (u_char) (size >> 16);
    b[2] = (u_char) (size >> 8);
    b[3] = (u_char) size;

    return ngx_live_rtmp_send(s, NGX_LIVE_RTMP_MSG_ACK_SIZE,
                              NGX_LIVE_RTMP_CSID_PROTOCOL, 0, 0, b, 4);
}


ngx_int_t
ngx_live_rtmp_send_set_peer_bandwidth(ngx_live_rtmp_session_t *s,
    uint32_t size, uint8_t type)
{
    u_char  b[5];

    b[0] = (u_char) (size >> 24);
    b[1] = (u_char) (size >> 16);
    b[2] = (u_char) (size >> 8);
    b[3] = (u_char) size;
    b[4] = type;

    return ngx_live_rtmp_send(s, NGX_LIVE_RTMP_MSG_BANDWIDTH,
                              NGX_LIVE_RTMP_CSID_PROTOCOL, 0, 0, b, 5);
}


ngx_int_t
ngx_live_rtmp_send_set_chunk_size(ngx_live_rtmp_session_t *s, uint32_t size)
{
    u_char  b[4];

    b[0] = (u_char) (size >> 24);
    b[1] = (u_char) (size >> 16);
    b[2] = (u_char) (size >> 8);
    b[3] = (u_char) size;

    return ngx_live_rtmp_send(s, NGX_LIVE_RTMP_MSG_CHUNK_SIZE,
                              NGX_LIVE_RTMP_CSID_PROTOCOL, 0, 0, b, 4);
}


ngx_int_t
ngx_live_rtmp_send_user_control(ngx_live_rtmp_session_t *s, uint16_t event,
    uint32_t data)
{
    u_char  b[6];

    b[0] = (u_char) (event >> 8);
    b[1] = (u_char) event;
    b[2] = (u_char) (data >> 24);
    b[3] = (u_char) (data >> 16);
    b[4] = (u_char) (data >> 8);
    b[5] = (u_char) data;

    return ngx_live_rtmp_send(s, NGX_LIVE_RTMP_MSG_USER,
                              NGX_LIVE_RTMP_CSID_PROTOCOL, 0, 0, b, 6);
}


/*
 * Finalization
 */

void
ngx_live_rtmp_finalize_session(ngx_live_rtmp_session_t *s)
{
    ngx_connection_t  *c;
    ngx_event_t       *e;

    c = s->connection;

    if (c->destroyed) {
        return;
    }

    c->destroyed = 1;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "live rtmp: finalize session");

    e = &s->close_ev;

    e->data = s;
    e->handler = ngx_live_rtmp_close_session_handler;
    e->log = c->log;

    ngx_post_event(e, &ngx_posted_events);
}


static void
ngx_live_rtmp_close_session_handler(ngx_event_t *ev)
{
    ngx_connection_t         *c;
    ngx_live_rtmp_session_t  *s;

    s = ev->data;
    c = s->connection;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "live rtmp: close session");

    /* publisher down: destroys the stream and kicks all subscribers */

    if (s->pub_stream) {
        ngx_live_rtmp_close_publisher(s);
    }

    /* player down: detach from the stream */

    if (!s->detaching && s->play_stream) {
        s->detaching = 1;
        ngx_live_stream_unsubscribe(s->play_stream, &s->sub);
    }

    s->play_stream = NULL;

    /* release queued shared references */

    while (s->out_head) {
        ngx_live_rtmp_out_node_t *node;

        node = s->out_head;
        s->out_head = node->next;

        ngx_live_rtmp_node_free(s, node);
    }

    if (c->read->timer_set) {
        ngx_del_timer(c->read);
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    ngx_live_rtmp_close_connection(c);
}


void
ngx_live_rtmp_close_connection(ngx_connection_t *c)
{
    ngx_pool_t  *pool;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "live rtmp: close connection");

    pool = c->pool;

    ngx_close_connection(c);

    ngx_destroy_pool(pool);
}


static u_int
ngx_live_rtmp_random(void)
{
    static u_int  seed = 0x9e3779b9;

    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;

    return seed;
}
