
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_RTMP_H_INCLUDED_
#define _NGX_LIVE_RTMP_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include "ngx_live.h"
#include "ngx_live_amf.h"
#include "ngx_live_shared.h"
#include "ngx_live_packet.h"
#include "ngx_live_stream.h"
#include "ngx_live_core_module.h"


#define NGX_LIVE_RTMP_VERSION              3
#define NGX_LIVE_RTMP_DEFAULT_CHUNK_SIZE   128
#define NGX_LIVE_RTMP_MAX_CHUNK_SIZE       0x00ffffff

/*
 * Chunk header: 3 basic + 11 message header + 4 extended timestamp,
 * also the headroom needed by outgoing messages.
 */
#define NGX_LIVE_RTMP_MAX_HEADER           18


/* RTMP message types */

#define NGX_LIVE_RTMP_MSG_CHUNK_SIZE       1
#define NGX_LIVE_RTMP_MSG_ABORT            2
#define NGX_LIVE_RTMP_MSG_ACK              3
#define NGX_LIVE_RTMP_MSG_USER             4
#define NGX_LIVE_RTMP_MSG_ACK_SIZE         5
#define NGX_LIVE_RTMP_MSG_BANDWIDTH        6
#define NGX_LIVE_RTMP_MSG_AUDIO            8
#define NGX_LIVE_RTMP_MSG_VIDEO            9
#define NGX_LIVE_RTMP_MSG_AMF3_META        15
#define NGX_LIVE_RTMP_MSG_AMF3_SHARED      16
#define NGX_LIVE_RTMP_MSG_AMF3_CMD         17
#define NGX_LIVE_RTMP_MSG_AMF_META         18
#define NGX_LIVE_RTMP_MSG_AMF_SHARED       19
#define NGX_LIVE_RTMP_MSG_AMF_CMD          20
#define NGX_LIVE_RTMP_MSG_AGGREGATE        22


/* user control event types */

#define NGX_LIVE_RTMP_USER_STREAM_BEGIN    0
#define NGX_LIVE_RTMP_USER_STREAM_EOF      1
#define NGX_LIVE_RTMP_USER_PING_REQUEST    6
#define NGX_LIVE_RTMP_USER_PING_RESPONSE   7


/* chunk stream ids used by the server */

#define NGX_LIVE_RTMP_CSID_PROTOCOL        2
#define NGX_LIVE_RTMP_CSID_INVOKE          3
#define NGX_LIVE_RTMP_CSID_AUDIO           4
#define NGX_LIVE_RTMP_CSID_VIDEO           5
#define NGX_LIVE_RTMP_CSID_DATA            6


typedef struct {
    uint32_t                csid;
    uint32_t                timestamp;   /* absolute, ms */
    uint32_t                mlen;
    uint8_t                 type;
    uint32_t                msid;
} ngx_live_rtmp_header_t;


/*
 * Per chunk-stream input state: the last message header (fmt1-3
 * carry deltas against it) and the message being reassembled.
 */

typedef struct {
    ngx_live_rtmp_header_t  hdr;
    uint32_t                dtime;       /* last timestamp delta */
    ngx_chain_t            *in;          /* reassembled payload */
    ngx_chain_t            *in_tail;
    size_t                  len;         /* bytes reassembled */
    unsigned                ext:1;       /* last header used ext timestamp */
} ngx_live_rtmp_istream_t;


/* input parser states */

#define NGX_LIVE_RTMP_ST_BASIC             0
#define NGX_LIVE_RTMP_ST_MSGHDR            1
#define NGX_LIVE_RTMP_ST_EXTTS             2
#define NGX_LIVE_RTMP_ST_PAYLOAD           3


/* one message in the output queue */

typedef struct ngx_live_rtmp_out_node_s {
    struct ngx_live_rtmp_out_node_s *next;

    ngx_chain_t            *shared;      /* acquired shared chain, or NULL */
    ngx_chain_t            *chain;       /* first link of the message */
    ngx_chain_t            *tail;        /* last link of the message */
    u_char                 *body;        /* control-message body block */
    size_t                  size;        /* payload bytes queued */
    u_char                  h[NGX_LIVE_RTMP_MAX_HEADER];
} ngx_live_rtmp_out_node_t;


/* recycled chain+buf pairs for output links */

typedef struct {
    ngx_chain_t             cl;
    ngx_buf_t               buf;
} ngx_live_rtmp_link_t;


/* handshake stages */

#define NGX_LIVE_RTMP_HS_C0C1              0
#define NGX_LIVE_RTMP_HS_C2                1
#define NGX_LIVE_RTMP_HS_DONE              2


typedef struct {
    ngx_int_t               chunk_size;
    ngx_msec_t              timeout;
    ngx_uint_t              max_streams;
    ngx_uint_t              ack_window;
    size_t                  max_message;
} ngx_live_rtmp_srv_conf_t;


typedef struct ngx_live_rtmp_session_s  ngx_live_rtmp_session_t;

/* per-listening server{} conf context */
typedef struct {
    ngx_live_conf_ctx_t    *ctx;
} ngx_live_rtmp_addr_conf_t;

struct ngx_live_rtmp_session_s {
    ngx_connection_t       *connection;
    ngx_live_conf_ctx_t    *ctx;          /* server{} conf context */
    ngx_live_rtmp_srv_conf_t *conf;

    ngx_live_core_app_conf_t *cacf;       /* resolved at connect() */

    /* connect() parameters (copies in the session pool) */

    ngx_str_t               app;
    ngx_str_t               args;
    ngx_str_t               flashver;
    ngx_str_t               tc_url;
    ngx_str_t               page_url;
    ngx_uint_t              object_encoding;
    uint32_t                acodecs;
    uint32_t                vcodecs;

    unsigned                connected:1;  /* connect() handled */

    /* stream ids */

    uint32_t                msid;         /* publish/play stream id */
    uint32_t                next_msid;

    /* publisher */

    ngx_live_stream_t      *pub_stream;

    /* player (embeds the protocol-agnostic subscriber) */

    ngx_live_stream_t      *play_stream;
    ngx_live_subscriber_t   sub;

    /* handshake */

    ngx_uint_t              hs_stage;
    ngx_buf_t              *hs_in;        /* C0+C1(+C2), 3073 bytes */
    ngx_buf_t              *hs_out;       /* S0+S1+S2, 3073 bytes */

    /* input chunk parser */

    ngx_uint_t              pstate;
    u_char                  basic[3];
    ngx_uint_t              basic_got;
    ngx_uint_t              basic_need;
    u_char                  mhdr[11];
    ngx_uint_t              mhdr_got;
    ngx_uint_t              mhdr_need;
    ngx_uint_t              pend_fmt;
    uint32_t                pend_csid;
    ngx_uint_t              pend_ext;

    ngx_uint_t              in_chunk_size;
    ngx_live_rtmp_istream_t *in_streams;
    size_t                  frag_pos;     /* payload consumed in this chunk */
    size_t                  frag_len;     /* size of this chunk */

    /* reassembled-message scratch buffers */

    ngx_chain_t            *in_free;      /* recycled 16K blocks */

    uint32_t                in_bytes;
    uint32_t                in_last_ack;
    uint32_t                in_ack_size;  /* peer's WindowAckSize */

    /* output queue */

    ngx_live_rtmp_out_node_t *out_head;
    ngx_live_rtmp_out_node_t *out_tail;
    ngx_chain_t            *out_link;     /* link being sent */
    u_char                 *out_pos;      /* position in it */
    size_t                  out_size;     /* pending payload bytes */
    ngx_uint_t              dropped;
    ngx_msec_t              last_progress;

    ngx_live_rtmp_out_node_t *free_nodes;
    ngx_live_rtmp_link_t     *free_links;
    u_char                  *free_bodies; /* control body blocks */

    ngx_event_t             close_ev;

    unsigned                wait_key:1;   /* hold video until a keyframe */
    unsigned                closed:1;
    unsigned                detaching:1;
};


/* listening handler, installed by the core module listen socket */
void ngx_live_rtmp_init_connection(ngx_connection_t *c);

/*
 * Queue one complete message.  body/len point at a contiguous buffer
 * (control and command messages); it is copied into the session.
 */
ngx_int_t ngx_live_rtmp_send(ngx_live_rtmp_session_t *s, uint8_t type,
    uint32_t csid, uint32_t msid, uint32_t timestamp,
    const u_char *body, size_t len);

/* queue one cached stream packet (shared chain, FLV-tag framed) */
ngx_int_t ngx_live_rtmp_send_packet(ngx_live_rtmp_session_t *s,
    uint32_t csid, uint32_t msid, ngx_live_packet_t *pkt);

/* protocol helpers */
ngx_int_t ngx_live_rtmp_send_ack(ngx_live_rtmp_session_t *s, uint32_t seq);
ngx_int_t ngx_live_rtmp_send_window_ack_size(ngx_live_rtmp_session_t *s,
    uint32_t size);
ngx_int_t ngx_live_rtmp_send_set_peer_bandwidth(ngx_live_rtmp_session_t *s,
    uint32_t size, uint8_t type);
ngx_int_t ngx_live_rtmp_send_set_chunk_size(ngx_live_rtmp_session_t *s,
    uint32_t size);
ngx_int_t ngx_live_rtmp_send_user_control(ngx_live_rtmp_session_t *s,
    uint16_t event, uint32_t data);

/* flush the output queue */
void ngx_live_rtmp_flush(ngx_live_rtmp_session_t *s);

/* release every queued output node (slow-consumer resync) */
void ngx_live_rtmp_node_free(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_out_node_t *node);

void ngx_live_rtmp_finalize_session(ngx_live_rtmp_session_t *s);


/* implemented in ngx_live_rtmp_cmd.c */

ngx_int_t ngx_live_rtmp_command(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_header_t *h, ngx_chain_t *in);
ngx_int_t ngx_live_rtmp_media(ngx_live_rtmp_session_t *s,
    ngx_live_rtmp_header_t *h, ngx_chain_t *in);
void ngx_live_rtmp_close_publisher(ngx_live_rtmp_session_t *s);


extern ngx_module_t ngx_live_rtmp_module;


#endif /* _NGX_LIVE_RTMP_H_INCLUDED_ */
