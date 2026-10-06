
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_STREAM_H_INCLUDED_
#define _NGX_LIVE_STREAM_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"
#include "ngx_live_packet.h"


#define NGX_LIVE_MAX_NAME        64
#define NGX_LIVE_STREAM_BUCKETS  256

/*
 * Hard cap on cached packets per GOP.  A normal GOP (x264/x265
 * default keyint=250 plus interleaved audio is ~700 packets) never
 * reaches it; it only guards against pathological streams without
 * keyframes (open-GOP/CRA sources), which would otherwise grow the
 * cache without bound.
 */
#define NGX_LIVE_GOP_MAX         4096


/*
 * A subscriber receives packets of a stream.  Protocol sessions
 * (HTTP-FLV player in S4, RTMP in P2) embed this as the first
 * member of their session structure and implement the callbacks.
 */

struct ngx_live_subscriber_s {
    ngx_queue_t                 queue;
    void                       *sess;

    /*
     * Send one packet (a complete FLV tag in pkt->chain).  The chain
     * is owned by the caller; a subscriber that keeps it past this
     * callback must acquire its own reference (and free it via the
     * app conf once the data was handed to the connection or dropped).
     *
     * Returning NGX_AGAIN/NGX_ERROR is advisory: the stream core
     * only logs it; backpressure policy belongs to the session.
     */
    ngx_int_t                 (*send)(void *sess, ngx_live_packet_t *pkt);

    /* the stream is gone: finish up and release the session */
    void                      (*close)(void *sess);
};


/*
 * One published stream.  Lives in the owning application's stream
 * hash; all memory comes from the app conf pool (cacf->pool), so
 * the stream outlives individual sessions.
 */

typedef struct {
    ngx_queue_t                 queue;
    ngx_live_packet_t           pkt;    /* chain acquired by node */
} ngx_live_gop_node_t;


struct ngx_live_stream_s {
    u_char                      name[NGX_LIVE_MAX_NAME];
    ngx_live_stream_t          *next;          /* hash chain */

    ngx_live_core_app_conf_t   *app_conf;

    /* opaque publisher session (HTTP request in S3, RTMP session in P2) */
    void                       *publisher;

    ngx_queue_t                 subscribers;   /* ngx_live_subscriber_t */
    ngx_uint_t                  nsubscribers;

    /* startup sequence cache (chains acquired) */
    ngx_live_packet_t          *meta;
    ngx_live_packet_t          *audio_header;
    ngx_live_packet_t          *video_header;

    /* GOP cache: rebuilt from each video keyframe */
    ngx_queue_t                 gop_cache;     /* ngx_live_gop_node_t */
    ngx_uint_t                  gop_count;

    ngx_msec_t                  epoch;         /* creation time */

    /* last dts per codec_type: [0] audio, [1] video */
    uint32_t                    last_dts[2];

    /* stats */
    uint64_t                    in_bytes;
    uint64_t                    nvideo;
    uint64_t                    naudio;

    unsigned                    publishing:1;
    unsigned                    active:1;      /* got first media packet */
};


/*
 * Stream registry.  Hash buckets live in cacf->streams; collisions
 * are chained through stream->next.
 *
 * Returns a pointer to the slot (so callers can unlink), or NULL
 * when not found and create == 0.
 */
ngx_live_stream_t **ngx_live_find_stream(ngx_live_core_app_conf_t *cacf,
    ngx_str_t *name, ngx_int_t create);

/*
 * Publish / unpublish.  publish fails with NGX_BUSY if the stream
 * is already being published.  ngx_live_stream_close() notifies all
 * subscribers, drops cached packets and recycles the stream.
 */
ngx_int_t ngx_live_stream_publish(ngx_live_stream_t *stream,
    void *publisher);
void ngx_live_stream_close(ngx_live_stream_t *stream);

/*
 * Feed one classified packet into the stream: updates the header /
 * meta / GOP caches and fans it out to all subscribers.  The caller
 * keeps ownership of pkt itself, but the chain must be a complete
 * FLV tag and is reference-counted by the stream core.
 */
void ngx_live_stream_packet(ngx_live_stream_t *stream,
    ngx_live_packet_t *pkt);

ngx_int_t ngx_live_stream_subscribe(ngx_live_stream_t *stream,
    ngx_live_subscriber_t *sub);
void ngx_live_stream_unsubscribe(ngx_live_stream_t *stream,
    ngx_live_subscriber_t *sub);


#endif /* _NGX_LIVE_STREAM_H_INCLUDED_ */
