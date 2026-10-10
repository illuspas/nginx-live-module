
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_WS_H_INCLUDED_
#define _NGX_LIVE_WS_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>


/*
 * Minimal RFC 6455 codec for the WebSocket-FLV transport
 * (docs/websocket-flv-support.md, phase 1).
 *
 * Outbound (server -> client) frames are never masked and never
 * fragmented: ngx_live_ws_frame_header() / ngx_live_ws_control_frame()
 * build them.
 *
 * The inbound parser consumes arbitrary chunks of a byte stream and
 * delivers payload fragments as they complete.  The FLV payload is a
 * byte stream in both directions (NMS compatible: frame boundaries
 * carry no meaning), so data frames are streamed fragment by fragment
 * instead of being buffered whole; control frames (ping/pong/close,
 * <= 125 bytes) are accumulated inside the parser and delivered
 * exactly once, complete.
 */


/* RFC 6455 1.3: the magic GUID appended to Sec-WebSocket-Key */
#define NGX_LIVE_WS_GUID             "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define NGX_LIVE_WS_GUID_LEN         (sizeof(NGX_LIVE_WS_GUID) - 1)

/* frame opcodes, RFC 6455 5.2 */
#define NGX_LIVE_WS_OP_CONT          0x00
#define NGX_LIVE_WS_OP_TEXT          0x01
#define NGX_LIVE_WS_OP_BINARY        0x02
#define NGX_LIVE_WS_OP_CLOSE         0x08
#define NGX_LIVE_WS_OP_PING          0x09
#define NGX_LIVE_WS_OP_PONG          0x0a

/* close status codes, RFC 6455 7.4.1 + private 4000-4999 range */
#define NGX_LIVE_WS_CLOSE_NORMAL     1000
#define NGX_LIVE_WS_CLOSE_GOING_AWAY 1001
#define NGX_LIVE_WS_CLOSE_PROTOCOL   1002
#define NGX_LIVE_WS_CLOSE_POLICY     1008
#define NGX_LIVE_WS_CLOSE_ERROR      1011
#define NGX_LIVE_WS_CLOSE_EXISTS     4001    /* stream already publishing */

/* base64(SHA1(key + GUID)) is always exactly 28 chars */
#define NGX_LIVE_WS_ACCEPT_LEN       28

/* sanity bound on the handshake key value (RFC sends 24) */
#define NGX_LIVE_WS_MAX_KEY          256

/* sanity bound on one frame payload (FLV tags stay far below) */
#define NGX_LIVE_WS_MAX_PAYLOAD      (64 * 1024 * 1024)

/* 7-bit encoded length limit, also the control payload limit (5.5) */
#define NGX_LIVE_WS_LEN7_MAX         125

/* header of one outbound frame: FIN + opcode + 7/16/64-bit length */
#define NGX_LIVE_WS_HEADER_MAX       10

/* complete outbound control frame: 2-byte header + <= 125 payload */
#define NGX_LIVE_WS_CTRL_MAX         (2 + NGX_LIVE_WS_LEN7_MAX)

/* fixed part of an inbound frame: 2 + ext length (8) + mask (4) */
#define NGX_LIVE_WS_FIXED_MAX        14


typedef struct {
    ngx_uint_t   state;         /* header / payload phase */

    u_char       opcode;        /* opcode of the frame being decoded */
    unsigned     fin:1;
    unsigned     ctrl:1;        /* control frame: buffered, complete */

    u_char       hdr[NGX_LIVE_WS_FIXED_MAX];
    size_t       hdr_len;       /* bytes accumulated in hdr */

    u_char       mask[4];
    size_t       mask_off;      /* next mask byte, 0..3 */

    uint64_t     left;          /* payload bytes left in this frame */

    u_char       ctrl_buf[NGX_LIVE_WS_LEN7_MAX];
    size_t       ctrl_len;      /* control payload bytes buffered */
} ngx_live_ws_parser_t;


/*
 * Payload delivery.  "payload" points into the caller's input buffer
 * (data frames) or into the parser's control buffer (control frames)
 * and is already unmasked; it may be NULL when "len" is 0.  Data
 * frames arrive as one or more fragments, "last" marks the final
 * one; control frames arrive exactly once, complete, with "last"
 * set.
 */

typedef void (*ngx_live_ws_payload_pt)(void *data, u_char opcode,
    u_char *payload, size_t len, ngx_uint_t last);


void ngx_live_ws_parser_init(ngx_live_ws_parser_t *wp);


/*
 * Consume len input bytes.  Returns NGX_OK, or NGX_ERROR on a
 * protocol violation (reserved RSV bits, reserved opcode,
 * fragmented or oversized control frame, unmasked client frame,
 * oversize payload); the caller must fail the session on error.
 */

ngx_int_t ngx_live_ws_parser_feed(ngx_live_ws_parser_t *wp, u_char *p,
    size_t len, ngx_live_ws_payload_pt cb, void *data);


/*
 * Compute Sec-WebSocket-Accept for the client key; writes exactly
 * NGX_LIVE_WS_ACCEPT_LEN chars plus a terminating NUL into dst
 * (at least NGX_LIVE_WS_ACCEPT_LEN + 1 bytes) and returns dst, or
 * returns NULL when the key length is out of bounds (malformed
 * handshake the caller may reject).
 */

u_char *ngx_live_ws_accept_key(const ngx_str_t *key, u_char *dst);


/*
 * Write the header of one outbound frame (FIN=1, never masked):
 * opcode plus a 7/16/64-bit length.  buf must hold
 * NGX_LIVE_WS_HEADER_MAX bytes; returns the number of bytes written.
 */

size_t ngx_live_ws_frame_header(u_char *buf, size_t len,
    ngx_uint_t opcode);


/*
 * Build one complete small control frame (pong, close, ...) into
 * buf; returns its total length, or 0 when it does not fit or the
 * payload exceeds NGX_LIVE_WS_LEN7_MAX.
 */

size_t ngx_live_ws_control_frame(u_char *buf, size_t size,
    ngx_uint_t opcode, const u_char *payload, size_t len);


#endif /* _NGX_LIVE_WS_H_INCLUDED_ */
