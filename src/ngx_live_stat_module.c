
/*
 * Copyright (C) illuspas
 *
 * Live statistics endpoint:
 *
 *     location /stat {
 *         live_stat on;
 *     }
 *
 * Serves a JSON document describing the applications and streams
 * served by this worker process.  The stream registry is per
 * worker, so with worker_processes > 1 each worker only reports
 * the streams it owns: poll /stat from every worker or run a
 * single worker for complete stats.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "ngx_live.h"
#include "ngx_live_packet.h"
#include "ngx_live_stream.h"
#include "ngx_live_core_module.h"


static ngx_int_t ngx_live_stat_handler(ngx_http_request_t *r);
static ngx_int_t ngx_live_stat_output(ngx_http_request_t *r);
static void *ngx_live_stat_create_loc_conf(ngx_conf_t *cf);
static char *ngx_live_stat_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *conf);
static char *ngx_live_stat(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);


typedef struct {
    ngx_flag_t      stat;    /* live_stat on|off */
} ngx_live_stat_loc_conf_t;


static ngx_command_t  ngx_live_stat_commands[] = {

    { ngx_string("live_stat"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_live_stat,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_live_stat_loc_conf_t, stat),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_live_stat_module_ctx = {
    NULL,                               /* preconfiguration */
    NULL,                               /* postconfiguration */

    NULL,                               /* create main configuration */
    NULL,                               /* init main configuration */

    NULL,                               /* create server configuration */
    NULL,                               /* merge server configuration */

    ngx_live_stat_create_loc_conf,      /* create location configuration */
    ngx_live_stat_merge_loc_conf        /* merge location configuration */
};


ngx_module_t  ngx_live_stat_module = {
    NGX_MODULE_V1,
    &ngx_live_stat_module_ctx,
    ngx_live_stat_commands,
    NGX_HTTP_MODULE,
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


/* upper bound for one stream entry, names included */
#define NGX_LIVE_STAT_STREAM_ROOM  640


static ngx_int_t
ngx_live_stat_handler(ngx_http_request_t *r)
{
    ngx_live_stat_loc_conf_t   *slcf;

    if (!(r->method & NGX_HTTP_GET)) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    slcf = ngx_http_get_module_loc_conf(r, ngx_live_stat_module);

    if (!slcf->stat) {
        return NGX_DECLINED;
    }

    if (r->uri.data[r->uri.len - 1] == '/') {
        return NGX_HTTP_NOT_FOUND;
    }

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_type_len = sizeof("application/json") - 1;
    r->headers_out.content_type.len = sizeof("application/json") - 1;
    r->headers_out.content_type.data = (u_char *) "application/json";

    return ngx_live_stat_output(r);
}


static u_char *
ngx_live_stat_escape_str(u_char *p, ngx_str_t *s)
{
    size_t   i;
    u_char   c;

    *p++ = '"';

    for (i = 0; i < s->len; i++) {
        c = s->data[i];

        if (c == '"' || c == '\\') {
            *p++ = '\\';
            *p++ = c;
            continue;
        }

        if (c < 0x20) {
            p = ngx_sprintf(p, "\\u%04uxD", (uint32_t) c);
            continue;
        }

        *p++ = c;
    }

    *p++ = '"';

    return p;
}


static ngx_int_t
ngx_live_stat_output(ngx_http_request_t *r)
{
    size_t                       room;
    u_char                      *p, *body;
    ngx_buf_t                   *b;
    ngx_chain_t                  out;
    ngx_int_t                    rc;
    ngx_uint_t                   n, i, nstreams;
    ngx_flag_t                   comma_app, comma_stream;
    ngx_live_stream_t           *stream;
    ngx_live_core_app_conf_t   **cacfp;
    ngx_live_core_main_conf_t   *cmcf;

    cmcf = ngx_live_core_main_conf;

    if (cmcf == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* upper-bound size calculation, then a single allocation */

    nstreams = 0;

    cacfp = cmcf->applications.elts;

    for (n = 0; n < cmcf->applications.nelts; n++) {
        for (i = 0; i < NGX_LIVE_STREAM_BUCKETS; i++) {
            for (stream = cacfp[n]->streams[i]; stream;
                 stream = stream->next)
            {
                nstreams++;
            }
        }
    }

    room = 256 + cmcf->applications.nelts * 128 + nstreams *
           NGX_LIVE_STAT_STREAM_ROOM;

    body = ngx_pnalloc(r->pool, room);
    if (body == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = body;

    p = ngx_sprintf(p,
                    "{\"nginx_version\":\"%s\","
                    "\"live_module_version\":\"%s\","
                    "\"worker_pid\":%P,"
                    "\"total_streams\":%ui,\"applications\":[",
                    NGINX_VERSION, NGX_LIVE_MODULE_VERSION,
                    ngx_pid, nstreams);

    comma_app = 0;

    for (n = 0; n < cmcf->applications.nelts; n++) {

        if (comma_app) {
            *p++ = ',';
        }
        comma_app = 1;

        p = ngx_sprintf(p, "{\"name\":");
        p = ngx_live_stat_escape_str(p, &cacfp[n]->name);
        p = ngx_sprintf(p, ",\"live\":%ui,\"streams\":[",
                        cacfp[n]->live);

        comma_stream = 0;

        for (i = 0; i < NGX_LIVE_STREAM_BUCKETS; i++) {

            for (stream = cacfp[n]->streams[i]; stream;
                 stream = stream->next)
            {
                u_char     vbuf[8], abuf[8];
                ngx_str_t  name, vname, aname;

                if (comma_stream) {
                    *p++ = ',';
                }
                comma_stream = 1;

                name.data = stream->name;
                name.len = ngx_strlen(stream->name);

                p = ngx_sprintf(p, "{\"name\":");
                p = ngx_live_stat_escape_str(p, &name);
                p = ngx_sprintf(p,
                                ",\"publishing\":%ui"
                                ",\"active\":%ui"
                                ",\"clients\":%ui"
                                ",\"video_codec\":",
                                stream->publishing,
                                stream->active,
                                stream->nsubscribers);

                if (stream->video_header) {
                    vname.data = ngx_live_codec_name(NGX_LIVE_VIDEO,
                                                     stream->video_header
                                                                  ->codec_id,
                                                     vbuf);
                    vname.len = ngx_strlen(vbuf);
                    p = ngx_live_stat_escape_str(p, &vname);

                } else {
                    *p++ = 'n'; *p++ = 'u'; *p++ = 'l'; *p++ = 'l';
                }

                p = ngx_sprintf(p, ",\"audio_codec\":");

                if (stream->audio_header) {
                    aname.data = ngx_live_codec_name(NGX_LIVE_AUDIO,
                                                     stream->audio_header
                                                                  ->codec_id,
                                                     abuf);
                    aname.len = ngx_strlen(abuf);
                    p = ngx_live_stat_escape_str(p, &aname);

                } else {
                    *p++ = 'n'; *p++ = 'u'; *p++ = 'l'; *p++ = 'l';
                }

                p = ngx_sprintf(p,
                                ",\"bytes_in\":%uL"
                                ",\"video_packets\":%uL"
                                ",\"audio_packets\":%uL"
                                ",\"gop_cache_packets\":%ui"
                                ",\"last_dts_video\":%uD"
                                ",\"last_dts_audio\":%uD"
                                ",\"uptime_ms\":%M}",
                                stream->in_bytes,
                                stream->nvideo,
                                stream->naudio,
                                stream->gop_count,
                                stream->last_dts[1],
                                stream->last_dts[0],
                                (ngx_msec_t) (ngx_current_msec
                                              - stream->epoch));
            }
        }

        p = ngx_sprintf(p, "]}");
    }

    p = ngx_sprintf(p, "]}");

    r->headers_out.content_length_n = p - body;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->pos = body;
    b->last = p;
    b->memory = 1;
    b->last_buf = 1;

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}


static void *
ngx_live_stat_create_loc_conf(ngx_conf_t *cf)
{
    ngx_live_stat_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_live_stat_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->stat = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_live_stat_merge_loc_conf(ngx_conf_t *cf, void *parent, void *conf)
{
    ngx_live_stat_loc_conf_t *prev = parent;
    ngx_live_stat_loc_conf_t *slcf = conf;

    ngx_conf_merge_value(slcf->stat, prev->stat, 0);

    if (slcf->stat == 0) {
        return NGX_CONF_OK;
    }

    if (ngx_live_core_main_conf == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "live_stat requires a \"live\" block "
                           "in the configuration");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_live_stat(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                      *rv;
    ngx_http_core_loc_conf_t  *clcf;

    rv = ngx_conf_set_flag_slot(cf, cmd, conf);
    if (rv != NGX_CONF_OK) {
        return rv;
    }

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_live_stat_handler;

    return NGX_CONF_OK;
}
