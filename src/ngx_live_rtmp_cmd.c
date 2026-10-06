
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <nginx.h>
#include "ngx_live.h"
#include "ngx_live_amf.h"
#include "ngx_live_rtmp.h"
#include "ngx_live_access_module.h"


#define NGX_LIVE_RTMP_OUT_LIMIT      1024 * 1024   /* 1M pending bytes */
#define NGX_LIVE_RTMP_STALL_MS       3000          /* slow consumer cut */

#define NGX_LIVE_RTMP_AMF_BUF        512


static ngx_int_t ngx_live_rtmp_connect(ngx_live_rtmp_session_t *s,
    double txn, ngx_live_amf_reader_t *r);
static ngx_int_t ngx_live_rtmp_create_stream(ngx_live_rtmp_session_t *s,
    double txn);
static ngx_int_t ngx_live_rtmp_publish(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_header_t *h, ngx_live_amf_reader_t *r);
static ngx_int_t ngx_live_rtmp_play(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_header_t *h, ngx_live_amf_reader_t *r);
static ngx_int_t ngx_live_rtmp_delete_stream(ngx_live_rtmp_session_t *s,
    ngx_live_amf_reader_t *r);

static ngx_int_t ngx_live_rtmp_player_send(void *sess,
    ngx_live_packet_t *pkt);
static void ngx_live_rtmp_player_close(void *sess);
static void ngx_live_rtmp_player_drain(ngx_live_rtmp_session_t *s);

static ngx_int_t ngx_live_rtmp_send_invoke(ngx_live_rtmp_session_t *s,
    uint32_t msid, u_char *buf, size_t len);
static ngx_int_t ngx_live_rtmp_send_status(ngx_live_rtmp_session_t *s,
    uint32_t msid, ngx_str_t *level, ngx_str_t *code, ngx_str_t *desc);
static ngx_int_t ngx_live_rtmp_send_connect_result(
    ngx_live_rtmp_session_t *s, double txn);
static ngx_int_t ngx_live_rtmp_send_create_stream_result(
    ngx_live_rtmp_session_t *s, double txn, uint32_t msid);
static ngx_int_t ngx_live_rtmp_send_sample_access(
    ngx_live_rtmp_session_t *s, uint32_t msid);

static ngx_live_core_app_conf_t *ngx_live_rtmp_find_application(
    ngx_str_t *app);
static ngx_int_t ngx_live_rtmp_safe_name(ngx_str_t *name);
static void ngx_live_rtmp_split_name(ngx_str_t *name, ngx_str_t *args);


/* AMF property names used in commands */

static ngx_str_t  amf_app_name         = ngx_string("app");
static ngx_str_t  amf_tc_url_name      = ngx_string("tcUrl");
static ngx_str_t  amf_flash_ver_name   = ngx_string("flashVer");
static ngx_str_t  amf_page_url_name    = ngx_string("pageUrl");
static ngx_str_t  amf_swf_url_name     = ngx_string("swfUrl");
static ngx_str_t  amf_object_enc_name  = ngx_string("objectEncoding");
static ngx_str_t  amf_audio_codecs_name = ngx_string("audioCodecs");
static ngx_str_t  amf_video_codecs_name = ngx_string("videoCodecs");

/* AMF strings used in responses */

static ngx_str_t  amf_result           = ngx_string("_result");
static ngx_str_t  amf_on_status        = ngx_string("onStatus");
static ngx_str_t  amf_fms_ver          = ngx_string("fmsVer");
static ngx_str_t  amf_fms_ver_value    = ngx_string("FMS/3,0,1,123");
static ngx_str_t  amf_capabilities     = ngx_string("capabilities");
static ngx_str_t  amf_level            = ngx_string("level");
static ngx_str_t  amf_code             = ngx_string("code");
static ngx_str_t  amf_description      = ngx_string("description");
static ngx_str_t  amf_status           = ngx_string("status");
static ngx_str_t  amf_error            = ngx_string("error");
static ngx_str_t  amf_sample_access    = ngx_string("|RtmpSampleAccess");

static ngx_str_t  amf_connect_success  =
    ngx_string("NetConnection.Connect.Success");
static ngx_str_t  amf_connect_rejected =
    ngx_string("NetConnection.Connect.Rejected");
static ngx_str_t  amf_publish_start    =
    ngx_string("NetStream.Publish.Start");
static ngx_str_t  amf_publish_badname  =
    ngx_string("NetStream.Publish.BadName");
static ngx_str_t  amf_play_reset       = ngx_string("NetStream.Play.Reset");
static ngx_str_t  amf_play_start       = ngx_string("NetStream.Play.Start");
static ngx_str_t  amf_play_notfound    =
    ngx_string("NetStream.Play.StreamNotFound");

static ngx_str_t  amf_desc_success     = ngx_string("Connection succeeded.");
static ngx_str_t  amf_desc_reset       =
    ngx_string("Playing and resetting stream.");
static ngx_str_t  amf_desc_start       = ngx_string("Started playing stream.");


/*
 * AMF0 command dispatch (message type 20).
 */

ngx_int_t
ngx_live_rtmp_command(ngx_live_rtmp_session_t *s, ngx_live_rtmp_header_t *h,
    ngx_chain_t *in)
{
    double                 txn;
    ngx_str_t              name;
    ngx_log_t             *log;
    ngx_live_amf_reader_t  r;

    log = s->connection->log;

    if (in == NULL) {
        return NGX_OK;
    }

    ngx_memzero(&name, sizeof(ngx_str_t));
    txn = 0;

    r.pool = s->connection->pool;
    r.log = log;
    r.cl = in;
    r.pos = in->buf->pos;
    r.last = in->buf->last;

    if (ngx_live_amf_read_cmd(&r, &name, &txn) != NGX_OK) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "live rtmp: malformed command message");
        return NGX_ERROR;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, 0,
                   "live rtmp: command \"%V\" txn=%.0f", &name, txn);

    if (name.len == sizeof("connect") - 1
        && ngx_strncmp(name.data, "connect", 7) == 0)
    {
        return ngx_live_rtmp_connect(s, txn, &r);
    }

    if (!s->connected) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "live rtmp: command \"%V\" before connect", &name);
        return NGX_ERROR;
    }

    if (name.len == sizeof("createStream") - 1
        && ngx_strncmp(name.data, "createStream", 12) == 0)
    {
        return ngx_live_rtmp_create_stream(s, txn);
    }

    if (name.len == sizeof("publish") - 1
        && ngx_strncmp(name.data, "publish", 7) == 0)
    {
        return ngx_live_rtmp_publish(s, h, &r);
    }

    if (name.len == sizeof("play") - 1
        && ngx_strncmp(name.data, "play", 4) == 0)
    {
        return ngx_live_rtmp_play(s, h, &r);
    }

    if (name.len == sizeof("deleteStream") - 1
        && ngx_strncmp(name.data, "deleteStream", 12) == 0)
    {
        return ngx_live_rtmp_delete_stream(s, &r);
    }

    if (name.len == sizeof("closeStream") - 1
        && ngx_strncmp(name.data, "closeStream", 11) == 0)
    {
        /* carries no stream id: stop whatever this session does */

        if (s->pub_stream) {
            ngx_live_rtmp_close_publisher(s);

        } else if (s->play_stream) {
            s->detaching = 1;
            ngx_live_stream_unsubscribe(s->play_stream, &s->sub);
            s->play_stream = NULL;
        }

        return NGX_OK;
    }

    if (name.len == sizeof("FCUnpublish") - 1
        && ngx_strncmp(name.data, "FCUnpublish", 11) == 0)
    {
        if (s->pub_stream) {
            ngx_live_rtmp_close_publisher(s);
        }

        return NGX_OK;
    }

    /*
     * releaseStream, FCPublish, getStreamLength, pause, seek and
     * friends are not needed for live relay; answering nothing works
     * with OBS, FFmpeg and librtmp clients (nginx-rtmp behaves the
     * same for unknown commands).
     */

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                   "live rtmp: ignoring command \"%V\"", &name);

    return NGX_OK;
}


/*
 * connect(<command object>)
 */

static ngx_int_t
ngx_live_rtmp_connect(ngx_live_rtmp_session_t *s, double txn,
    ngx_live_amf_reader_t *r)
{
    ngx_str_t               app;
    ngx_log_t              *log;
    ngx_live_amf_elt_t      top[1];

    struct {
        ngx_str_t   app;
        ngx_str_t   tc_url;
        ngx_str_t   flashver;
        ngx_str_t   page_url;
        ngx_str_t   swf_url;
        double      object_encoding;
        double      audio_codecs;
        double      video_codecs;
    } v;

    ngx_live_amf_elt_t      props[] = {
        { NGX_LIVE_AMF_STRING, &amf_app_name,          NULL, 0, 0 },
        { NGX_LIVE_AMF_STRING, &amf_tc_url_name,       NULL, 0, 0 },
        { NGX_LIVE_AMF_STRING, &amf_flash_ver_name,    NULL, 0, 0 },
        { NGX_LIVE_AMF_STRING, &amf_page_url_name,     NULL, 0, 0 },
        { NGX_LIVE_AMF_STRING, &amf_swf_url_name,      NULL, 0, 0 },
        { NGX_LIVE_AMF_NUMBER, &amf_object_enc_name,   NULL, 0, 0 },
        { NGX_LIVE_AMF_NUMBER, &amf_audio_codecs_name, NULL, 0, 0 },
        { NGX_LIVE_AMF_NUMBER, &amf_video_codecs_name, NULL, 0, 0 },
    };

    log = s->connection->log;

    if (s->connected) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "live rtmp: duplicate connect");
        return NGX_ERROR;
    }

    ngx_memzero(&v, sizeof(v));

    props[0].data = &v.app;
    props[1].data = &v.tc_url;
    props[2].data = &v.flashver;
    props[3].data = &v.page_url;
    props[4].data = &v.swf_url;
    props[5].data = &v.object_encoding;
    props[6].data = &v.audio_codecs;
    props[7].data = &v.video_codecs;

    top[0].type = NGX_LIVE_AMF_OBJECT;
    top[0].name = NULL;
    top[0].data = props;
    top[0].len = sizeof(props) / sizeof(props[0]);
    top[0].mandatory = 0;

    if (ngx_live_amf_read(r, top, 1) != NGX_OK) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "live rtmp: malformed connect");
        return NGX_ERROR;
    }

    /*
     * The app parameter may carry an instance suffix or a query
     * ("myapp", "myapp/_definst_", "myapp?x=1"); match on the first
     * path segment.
     */

    app = v.app;

    {
        size_t  n;

        for (n = 0; n < app.len; n++) {
            if (app.data[n] == '/' || app.data[n] == '?') {
                break;
            }
        }

        app.len = n;
    }

    if (app.len == 0) {
        goto rejected;
    }

    s->cacf = ngx_live_rtmp_find_application(&app);

    if (s->cacf == NULL || s->cacf->live == 0) {
        goto rejected;
    }

    s->app = v.app;
    s->tc_url = v.tc_url;
    s->flashver = v.flashver;
    s->page_url = v.page_url;
    s->object_encoding = (ngx_uint_t) v.object_encoding;
    s->acodecs = (uint32_t) v.audio_codecs;
    s->vcodecs = (uint32_t) v.video_codecs;

    s->connected = 1;

    ngx_log_error(NGX_LOG_INFO, log, 0,
                  "live rtmp: connect app=%V flashver=%V",
                  &s->app, &s->flashver);

    /* protocol handshake per NMS/FMS: window -> bandwidth -> chunks */

    if (ngx_live_rtmp_send_window_ack_size(s, s->conf->ack_window) != NGX_OK
        || ngx_live_rtmp_send_set_peer_bandwidth(s, s->conf->ack_window, 2)
           != NGX_OK
        || ngx_live_rtmp_send_set_chunk_size(s, s->conf->chunk_size)
           != NGX_OK)
    {
        return NGX_ERROR;
    }

    return ngx_live_rtmp_send_connect_result(s, txn);

rejected:

    ngx_log_error(NGX_LOG_INFO, log, 0,
                  "live rtmp: connect rejected, app=%V", &v.app);

    s->connected = 1;    /* let the status message out */

    (void) ngx_live_rtmp_send_status(s, 0, &amf_error, &amf_connect_rejected,
                                     &app);

    ngx_live_rtmp_finalize_session(s);

    return NGX_OK;
}


static ngx_int_t
ngx_live_rtmp_create_stream(ngx_live_rtmp_session_t *s, double txn)
{
    s->next_msid++;

    return ngx_live_rtmp_send_create_stream_result(s, txn, s->next_msid);
}


/*
 * publish(<name>, <type>)
 */

static ngx_int_t
ngx_live_rtmp_publish(ngx_live_rtmp_session_t *s, ngx_live_rtmp_header_t *h,
    ngx_live_amf_reader_t *r)
{
    ngx_str_t               name, args, desc;
    ngx_log_t              *log;
    ngx_live_stream_t      **sp;
    ngx_live_amf_elt_t      top[] = {
        { NGX_LIVE_AMF_ANY,    NULL, NULL, 0, 0 },   /* command object */
        { NGX_LIVE_AMF_STRING, NULL, NULL, 0, 1 },   /* stream name */
        { NGX_LIVE_AMF_ANY,    NULL, NULL, 0, 0 },   /* publishing type */
    };

    log = s->connection->log;

    ngx_memzero(&name, sizeof(ngx_str_t));
    top[1].data = &name;

    if (ngx_live_amf_read(r, top, 3) != NGX_OK) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "live rtmp: malformed publish");
        return NGX_ERROR;
    }

    ngx_live_rtmp_split_name(&name, &args);

    if (s->pub_stream) {
        goto bad;
    }

    if (!ngx_live_rtmp_safe_name(&name)) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "live rtmp: unsafe stream name in publish");
        goto bad;
    }

    if (s->cacf->live == 0) {
        goto bad;
    }

    if (ngx_live_access_permit(s->cacf, s->connection,
                               NGX_LIVE_ACCESS_PUBLISH) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "live rtmp: publish denied by access rules");
        goto bad;
    }

    sp = ngx_live_find_stream(s->cacf, &name, 1);

    if (sp == NULL) {
        return NGX_ERROR;
    }

    if (ngx_live_stream_publish(*sp, s) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "live rtmp: stream \"%V\" already publishing", &name);
        goto bad;
    }

    s->pub_stream = *sp;
    s->msid = h->msid;

    if (s->conf->timeout) {
        ngx_add_timer(s->connection->read, s->conf->timeout);
    }

    (void) ngx_live_rtmp_send_user_control(s,
                                    NGX_LIVE_RTMP_USER_STREAM_BEGIN, s->msid);

    desc.data = NULL;
    desc.len = 0;

    (void) ngx_live_rtmp_send_status(s, s->msid, &amf_status,
                                     &amf_publish_start, &desc);

    ngx_log_error(NGX_LOG_INFO, log, 0,
                  "live rtmp: publish started, app=%V stream=%V",
                  &s->cacf->name, &name);

    return NGX_OK;

bad:

    (void) ngx_live_rtmp_send_status(s, h->msid, &amf_error,
                                     &amf_publish_badname, &name);

    ngx_live_rtmp_finalize_session(s);

    return NGX_OK;
}


/*
 * play(<name>, [start], [duration], [reset])
 */

static ngx_int_t
ngx_live_rtmp_play(ngx_live_rtmp_session_t *s, ngx_live_rtmp_header_t *h,
    ngx_live_amf_reader_t *r)
{
    ngx_str_t               name, args;
    ngx_log_t              *log;
    ngx_queue_t            *item;
    ngx_live_stream_t      **sp;
    ngx_live_gop_node_t    *node;
    ngx_live_amf_elt_t      top[] = {
        { NGX_LIVE_AMF_ANY,    NULL, NULL, 0, 0 },   /* command object */
        { NGX_LIVE_AMF_STRING, NULL, NULL, 0, 1 },   /* stream name */
        { NGX_LIVE_AMF_ANY,    NULL, NULL, 0, 0 },   /* start */
        { NGX_LIVE_AMF_ANY,    NULL, NULL, 0, 0 },   /* duration */
        { NGX_LIVE_AMF_ANY,    NULL, NULL, 0, 0 },   /* reset */
    };

    log = s->connection->log;

    ngx_memzero(&name, sizeof(ngx_str_t));
    top[1].data = &name;

    if (ngx_live_amf_read(r, top, 5) != NGX_OK) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "live rtmp: malformed play");
        return NGX_ERROR;
    }

    ngx_live_rtmp_split_name(&name, &args);

    if (!ngx_live_rtmp_safe_name(&name)) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "live rtmp: unsafe stream name in play");
        goto notfound;
    }

    if (s->cacf->live == 0) {
        goto notfound;
    }

    if (ngx_live_access_permit(s->cacf, s->connection,
                               NGX_LIVE_ACCESS_PLAY) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "live rtmp: play denied by access rules");
        goto notfound;
    }

    sp = ngx_live_find_stream(s->cacf, &name, 0);

    if (sp == NULL) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "live rtmp: stream \"%V\" not found", &name);
        goto notfound;
    }

    if (s->play_stream) {
        /* one play per session */
        s->detaching = 1;
        ngx_live_stream_unsubscribe(s->play_stream, &s->sub);
        s->play_stream = NULL;
        s->detaching = 0;
    }

    s->play_stream = *sp;
    s->msid = h->msid;

    s->sub.sess = s;
    s->sub.send = ngx_live_rtmp_player_send;
    s->sub.close = ngx_live_rtmp_player_close;

    (void) ngx_live_rtmp_send_user_control(s,
                                    NGX_LIVE_RTMP_USER_STREAM_BEGIN, s->msid);

    (void) ngx_live_rtmp_send_status(s, s->msid, &amf_status,
                                     &amf_play_reset, &amf_desc_reset);

    (void) ngx_live_rtmp_send_status(s, s->msid, &amf_status,
                                     &amf_play_start, &amf_desc_start);

    (void) ngx_live_rtmp_send_sample_access(s, s->msid);

    /*
     * Startup sequence: cached meta, codec headers and the GOP cache
     * (no 13-byte FLV file header here, RTMP carries the same bodies
     * as message payloads).
     */

    if ((*sp)->meta) {
        (void) ngx_live_rtmp_send_packet(s, NGX_LIVE_RTMP_CSID_DATA,
                                         s->msid, (*sp)->meta);
    }

    if ((*sp)->audio_header) {
        (void) ngx_live_rtmp_send_packet(s, NGX_LIVE_RTMP_CSID_AUDIO,
                                         s->msid, (*sp)->audio_header);
    }

    if ((*sp)->video_header) {
        (void) ngx_live_rtmp_send_packet(s, NGX_LIVE_RTMP_CSID_VIDEO,
                                         s->msid, (*sp)->video_header);
    }

    if (ngx_queue_empty(&(*sp)->gop_cache)) {
        s->wait_key = 1;
    }

    for (item = ngx_queue_head(&(*sp)->gop_cache);
         item != ngx_queue_sentinel(&(*sp)->gop_cache);
         item = ngx_queue_next(item))
    {
        node = ngx_queue_data(item, ngx_live_gop_node_t, queue);

        (void) ngx_live_rtmp_send_packet(s,
            node->pkt.codec_type == NGX_LIVE_AUDIO
                ? NGX_LIVE_RTMP_CSID_AUDIO : NGX_LIVE_RTMP_CSID_VIDEO,
            s->msid, &node->pkt);
    }

    ngx_live_stream_subscribe(*sp, &s->sub);

    ngx_log_error(NGX_LOG_INFO, log, 0,
                  "live rtmp: play started, app=%V stream=%V "
                  "(gop=%ui subscribers=%ui)",
                  &s->cacf->name, &name, (*sp)->gop_count,
                  (*sp)->nsubscribers);

    return NGX_OK;

notfound:

    (void) ngx_live_rtmp_send_status(s, h->msid, &amf_error,
                                     &amf_play_notfound, &name);

    ngx_live_rtmp_finalize_session(s);

    return NGX_OK;
}


/*
 * deleteStream(<stream id>)
 */

static ngx_int_t
ngx_live_rtmp_delete_stream(ngx_live_rtmp_session_t *s,
    ngx_live_amf_reader_t *r)
{
    double              msid = 0;
    ngx_live_amf_elt_t  top[] = {
        { NGX_LIVE_AMF_ANY,    NULL, NULL, 0, 0 },
        { NGX_LIVE_AMF_NUMBER, NULL, &msid, 0, 0 },
    };

    if (ngx_live_amf_read(r, top, 2) != NGX_OK) {
        return NGX_OK;
    }

    if (s->pub_stream && s->msid == (uint32_t) msid) {
        ngx_live_rtmp_close_publisher(s);
        return NGX_OK;
    }

    if (s->play_stream && s->msid == (uint32_t) msid) {
        s->detaching = 1;
        ngx_live_stream_unsubscribe(s->play_stream, &s->sub);
        s->play_stream = NULL;
    }

    return NGX_OK;
}


/*
 * Media path (publisher): message payload == FLV tag body.
 */

ngx_int_t
ngx_live_rtmp_media(ngx_live_rtmp_session_t *s, ngx_live_rtmp_header_t *h,
    ngx_chain_t *in)
{
    ngx_live_packet_t   pkt;

    if (s->pub_stream == NULL) {
        /* sessions that do not publish have no business sending media */
        return NGX_OK;
    }

    if (in == NULL || h->mlen == 0) {
        return NGX_OK;
    }

    ngx_memzero(&pkt, sizeof(ngx_live_packet_t));

    pkt.chain = ngx_live_append_shared_bufs(s->cacf, NULL, in);

    if (pkt.chain == NULL) {
        return NGX_ERROR;
    }

    pkt.codec_type = h->type;
    pkt.dts = h->timestamp;
    pkt.size = h->mlen;

    if (ngx_live_packet_identify(&pkt) != NGX_OK) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, s->connection->log, 0,
                       "live rtmp: malformed %ui message dropped",
                       (ngx_uint_t) h->type);
        ngx_live_free_shared_chain(s->cacf, pkt.chain);
        return NGX_OK;
    }

    ngx_live_packet_prepend_tag_header(&pkt);

    ngx_live_stream_packet(s->pub_stream, &pkt);

    /* release this session's own reference */

    ngx_live_free_shared_chain(s->cacf, pkt.chain);

    return NGX_OK;
}


void
ngx_live_rtmp_close_publisher(ngx_live_rtmp_session_t *s)
{
    ngx_live_stream_t  *stream;

    if (s->pub_stream == NULL) {
        return;
    }

    if (s->connection->read->timer_set) {
        ngx_del_timer(s->connection->read);
    }

    stream = s->pub_stream;
    s->pub_stream = NULL;

    ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                  "live rtmp: publish stopped, stream=%s", stream->name);

    /* closes the stream and kicks all subscribers */

    ngx_live_stream_close(stream);
}


/*
 * Player callbacks (stream core subscriber interface).
 */

static ngx_int_t
ngx_live_rtmp_player_send(void *sess, ngx_live_packet_t *pkt)
{
    uint32_t                    csid;
    ngx_live_rtmp_session_t    *s = sess;

    if (s->play_stream == NULL) {
        return NGX_OK;
    }

    /* mid-GOP start: hold video until the stream reaches a keyframe */

    if (s->wait_key) {
        if (pkt->flags == NGX_LIVE_FLAG_VIDEO_KEY) {
            s->wait_key = 0;

        } else if (pkt->flags == NGX_LIVE_FLAG_VIDEO_INTER) {
            return NGX_OK;
        }
    }

    /* backpressure: same policy as the HTTP-FLV player */

    if (s->out_size > NGX_LIVE_RTMP_OUT_LIMIT) {

        if (ngx_current_msec - s->last_progress > NGX_LIVE_RTMP_STALL_MS) {
            ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                          "live rtmp: slow consumer dropped "
                          "(out=%uz dropped=%ui)",
                          s->out_size, s->dropped);
            ngx_live_rtmp_finalize_session(s);
            return NGX_OK;
        }

        if (pkt->flags == NGX_LIVE_FLAG_VIDEO_KEY) {
            /* resync from this keyframe: drop everything queued */
            ngx_live_rtmp_player_drain(s);

        } else if (pkt->flags == NGX_LIVE_FLAG_AUDIO
                   || pkt->flags == NGX_LIVE_FLAG_VIDEO_INTER)
        {
            s->dropped++;
            return NGX_OK;
        }
    }

    switch (pkt->codec_type) {

    case NGX_LIVE_AUDIO:
        csid = NGX_LIVE_RTMP_CSID_AUDIO;
        break;

    case NGX_LIVE_VIDEO:
        csid = NGX_LIVE_RTMP_CSID_VIDEO;
        break;

    default:
        csid = NGX_LIVE_RTMP_CSID_DATA;
        break;
    }

    return ngx_live_rtmp_send_packet(s, csid, s->msid, pkt);
}


static void
ngx_live_rtmp_player_close(void *sess)
{
    ngx_live_rtmp_session_t  *s = sess;

    /* the stream core has already unlinked the subscriber node */

    s->detaching = 1;
    s->play_stream = NULL;

    ngx_live_rtmp_finalize_session(s);
}


static void
ngx_live_rtmp_player_drain(ngx_live_rtmp_session_t *s)
{
    while (s->out_head) {
        ngx_live_rtmp_out_node_t *node;

        node = s->out_head;
        s->out_head = node->next;

        ngx_live_rtmp_node_free(s, node);
    }

    s->out_tail = NULL;
    s->out_link = NULL;
    s->out_size = 0;
}


/*
 * AMF response builders
 */

static ngx_int_t
ngx_live_rtmp_send_invoke(ngx_live_rtmp_session_t *s, uint32_t msid,
    u_char *buf, size_t len)
{
    return ngx_live_rtmp_send(s, NGX_LIVE_RTMP_MSG_AMF_CMD,
                              NGX_LIVE_RTMP_CSID_INVOKE, msid, 0, buf, len);
}


static ngx_int_t
ngx_live_rtmp_send_status(ngx_live_rtmp_session_t *s, uint32_t msid,
    ngx_str_t *level, ngx_str_t *code, ngx_str_t *desc)
{
    u_char                  buf[NGX_LIVE_RTMP_AMF_BUF];
    ngx_live_amf_writer_t   w;

    w.p = buf;
    w.end = buf + sizeof(buf);

    ngx_live_amf_write_string(&w, &amf_on_status);
    ngx_live_amf_write_number(&w, 0);
    ngx_live_amf_write_null(&w);

    ngx_live_amf_write_object_begin(&w);
    ngx_live_amf_write_name(&w, &amf_level);
    ngx_live_amf_write_string(&w, level);
    ngx_live_amf_write_name(&w, &amf_code);
    ngx_live_amf_write_string(&w, code);
    ngx_live_amf_write_name(&w, &amf_description);
    ngx_live_amf_write_string(&w, desc);
    ngx_live_amf_write_object_end(&w);

    if (ngx_live_amf_writer_full(&w)) {
        return NGX_ERROR;
    }

    return ngx_live_rtmp_send_invoke(s, msid, buf, w.p - buf);
}


static ngx_int_t
ngx_live_rtmp_send_connect_result(ngx_live_rtmp_session_t *s, double txn)
{
    u_char                  buf[NGX_LIVE_RTMP_AMF_BUF];
    ngx_live_amf_writer_t   w;

    w.p = buf;
    w.end = buf + sizeof(buf);

    ngx_live_amf_write_string(&w, &amf_result);
    ngx_live_amf_write_number(&w, txn);
    ngx_live_amf_write_null(&w);

    /* properties */

    ngx_live_amf_write_object_begin(&w);
    ngx_live_amf_write_name(&w, &amf_fms_ver);
    ngx_live_amf_write_string(&w, &amf_fms_ver_value);
    ngx_live_amf_write_name(&w, &amf_capabilities);
    ngx_live_amf_write_number(&w, 31);
    ngx_live_amf_write_object_end(&w);

    /* information */

    ngx_live_amf_write_object_begin(&w);
    ngx_live_amf_write_name(&w, &amf_level);
    ngx_live_amf_write_string(&w, &amf_status);
    ngx_live_amf_write_name(&w, &amf_code);
    ngx_live_amf_write_string(&w, &amf_connect_success);
    ngx_live_amf_write_name(&w, &amf_description);
    ngx_live_amf_write_string(&w, &amf_desc_success);
    ngx_live_amf_write_name(&w, &amf_object_enc_name);
    ngx_live_amf_write_number(&w, 0);
    ngx_live_amf_write_object_end(&w);

    if (ngx_live_amf_writer_full(&w)) {
        return NGX_ERROR;
    }

    return ngx_live_rtmp_send_invoke(s, 0, buf, w.p - buf);
}


static ngx_int_t
ngx_live_rtmp_send_create_stream_result(ngx_live_rtmp_session_t *s,
    double txn, uint32_t msid)
{
    u_char                  buf[NGX_LIVE_RTMP_AMF_BUF];
    ngx_live_amf_writer_t   w;

    w.p = buf;
    w.end = buf + sizeof(buf);

    ngx_live_amf_write_string(&w, &amf_result);
    ngx_live_amf_write_number(&w, txn);
    ngx_live_amf_write_null(&w);
    ngx_live_amf_write_number(&w, msid);

    if (ngx_live_amf_writer_full(&w)) {
        return NGX_ERROR;
    }

    return ngx_live_rtmp_send_invoke(s, 0, buf, w.p - buf);
}


static ngx_int_t
ngx_live_rtmp_send_sample_access(ngx_live_rtmp_session_t *s, uint32_t msid)
{
    u_char                  buf[NGX_LIVE_RTMP_AMF_BUF];
    ngx_live_amf_writer_t   w;

    w.p = buf;
    w.end = buf + sizeof(buf);

    ngx_live_amf_write_string(&w, &amf_sample_access);
    ngx_live_amf_write_boolean(&w, 1);
    ngx_live_amf_write_boolean(&w, 1);

    if (ngx_live_amf_writer_full(&w)) {
        return NGX_ERROR;
    }

    return ngx_live_rtmp_send(s, NGX_LIVE_RTMP_MSG_AMF_META,
                              NGX_LIVE_RTMP_CSID_DATA, msid, 0,
                              buf, (size_t) (w.p - buf));
}


/*
 * Helpers
 */

static ngx_live_core_app_conf_t *
ngx_live_rtmp_find_application(ngx_str_t *app)
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
ngx_live_rtmp_safe_name(ngx_str_t *name)
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


/* "stream?key=value" -> name "stream", args "key=value" */

static void
ngx_live_rtmp_split_name(ngx_str_t *name, ngx_str_t *args)
{
    u_char  *p;

    args->data = NULL;
    args->len = 0;

    p = ngx_strlchr(name->data, name->data + name->len, '?');

    if (p) {
        args->data = p + 1;
        args->len = name->data + name->len - (p + 1);
        name->len = p - name->data;
    }
}
