
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_PACKET_H_INCLUDED_
#define _NGX_LIVE_PACKET_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"


/*
 * The unified media packet.  The payload (pkt->chain) is a complete
 * FLV tag: an 11-byte tag header prepended in the shared buffer
 * headroom, followed by the tag body (== RTMP message payload,
 * byte-identical).
 *
 * Ownership: the chain is reference-counted.  Every long-term holder
 * (stream header cache, GOP cache, subscriber output queue) must
 * acquire it and free its own reference when done.
 */

typedef struct {
    ngx_uint_t       codec_type;    /* NGX_LIVE_AUDIO/VIDEO/SCRIPT */
    ngx_uint_t       flags;         /* NGX_LIVE_FLAG_* */
    uint32_t         codec_id;      /* legacy CodecID or FourCC uint32 */
    uint32_t         dts;           /* milliseconds */
    int32_t          cts;           /* composition time, pts = dts + cts */
    size_t           size;          /* tag body size, w/o 11-byte header */
    ngx_chain_t     *chain;         /* complete FLV tag data */
} ngx_live_packet_t;


/*
 * Read up to n bytes from the head of a chain into dst without
 * consuming them.  Returns the number of bytes actually read.
 */
size_t ngx_live_chain_read(ngx_chain_t *in, u_char *dst, size_t n);


/*
 * Classify a raw tag body: fills flags, codec_id and cts of pkt.
 * codec_type, dts, size and chain must be set by the caller before.
 *
 * The chain passed here carries only the tag body (no 11-byte
 * header prepended yet).  Covers legacy FLV codecs and Enhanced
 * RTMP v1 (video: av01/vp09/hvc1, audio: ac-3/ec-3/Opus/.mp3/fLaC).
 *
 * Returns NGX_OK, or NGX_ERROR on malformed data.
 */
ngx_int_t ngx_live_packet_identify(ngx_live_packet_t *pkt);


/*
 * Codec id to printable name (FourCC shown as ASCII).  buf must
 * have room for at least 8 bytes.  Used by the stat module.
 */
u_char *ngx_live_codec_name(ngx_uint_t codec_type, uint32_t codec_id,
    u_char *buf);


/*
 * Prepend the 11-byte FLV tag header into the headroom of the first
 * buffer of pkt->chain.  After this call the chain is a complete
 * FLV tag ready for caching and fan-out.
 */
void ngx_live_packet_prepend_tag_header(ngx_live_packet_t *pkt);


#endif /* _NGX_LIVE_PACKET_H_INCLUDED_ */
