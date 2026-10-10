
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_sha1.h>

#include "ngx_live_ws.h"


/* parser phases */

#define NGX_LIVE_WS_ST_HEADER    0
#define NGX_LIVE_WS_ST_PAYLOAD   1


static size_t ngx_live_ws_header_need(ngx_live_ws_parser_t *wp);
static void ngx_live_ws_unmask(u_char *p, size_t len, u_char *mask,
    size_t *off);


void
ngx_live_ws_parser_init(ngx_live_ws_parser_t *wp)
{
    ngx_memzero(wp, sizeof(ngx_live_ws_parser_t));
}


/*
 * Bytes that complete the fixed part of the frame header currently
 * being accumulated: 2 fixed + extended length + mask.
 */

static size_t
ngx_live_ws_header_need(ngx_live_ws_parser_t *wp)
{
    size_t  need;

    need = 2;

    if (wp->hdr_len >= 2) {

        switch (wp->hdr[1] & 0x7f) {

        case 127:
            need += 8;
            break;

        case 126:
            need += 2;
            break;

        default:
            break;
        }

        if (wp->hdr[1] & 0x80) {
            need += 4;
        }
    }

    return need;
}


static void
ngx_live_ws_unmask(u_char *p, size_t len, u_char *mask, size_t *off)
{
    size_t  i;

    for (i = 0; i < len; i++) {
        p[i] ^= mask[*off];
        *off = (*off + 1) & 3;
    }
}


ngx_int_t
ngx_live_ws_parser_feed(ngx_live_ws_parser_t *wp, u_char *p, size_t len,
    ngx_live_ws_payload_pt cb, void *data)
{
    size_t    ext, i, n, need;
    u_char    b0, b1, *payload;
    uint64_t  flen;

    while (len) {

        if (wp->state == NGX_LIVE_WS_ST_HEADER) {

            /*
             * Accumulate the fixed part (2 + ext length + mask).
             * The requirement grows as hdr[1] arrives, so recompute
             * it after every copy until the fixed part is complete.
             */

            for ( ;; ) {

                need = ngx_live_ws_header_need(wp);

                if (wp->hdr_len == need) {
                    break;
                }

                n = ngx_min(need - wp->hdr_len, len);

                if (n == 0) {
                    return NGX_OK;    /* input exhausted */
                }

                ngx_memcpy(wp->hdr + wp->hdr_len, p, n);
                wp->hdr_len += n;
                p += n;
                len -= n;
            }

            b0 = wp->hdr[0];
            b1 = wp->hdr[1];

            if (b0 & 0x70) {
                /* RSV1-3: no extension is negotiated (6.1) */

                return NGX_ERROR;
            }

            wp->fin = (b0 & 0x80) ? 1 : 0;
            wp->opcode = b0 & 0x0f;

            switch (wp->opcode) {

            case NGX_LIVE_WS_OP_CONT:
            case NGX_LIVE_WS_OP_TEXT:
            case NGX_LIVE_WS_OP_BINARY:
                wp->ctrl = 0;
                break;

            case NGX_LIVE_WS_OP_CLOSE:
            case NGX_LIVE_WS_OP_PING:
            case NGX_LIVE_WS_OP_PONG:

                /*
                 * RFC 6455 5.5: control frames are never fragmented
                 * and never longer than 125 bytes
                 */

                if (!wp->fin || (b1 & 0x7f) > NGX_LIVE_WS_LEN7_MAX) {
                    return NGX_ERROR;
                }

                wp->ctrl = 1;
                break;

            default:
                return NGX_ERROR;    /* reserved opcode */
            }

            if (!(b1 & 0x80)) {
                /* RFC 6455 5.1: client-to-server frames MUST be masked */

                return NGX_ERROR;
            }

            ext = 0;
            flen = b1 & 0x7f;

            switch (flen) {

            case 126:
                ext = 2;
                flen = (wp->hdr[2] << 8) | wp->hdr[3];
                break;

            case 127:
                ext = 8;
                flen = 0;

                for (i = 0; i < 8; i++) {
                    flen = (flen << 8) | wp->hdr[2 + i];
                }

                break;

            default:
                break;
            }

            if (flen > NGX_LIVE_WS_MAX_PAYLOAD) {
                return NGX_ERROR;
            }

            wp->left = flen;
            wp->mask_off = 0;
            wp->ctrl_len = 0;
            ngx_memcpy(wp->mask, wp->hdr + 2 + ext, 4);

            wp->state = NGX_LIVE_WS_ST_PAYLOAD;

            if (flen == 0) {
                /* empty frame: no payload phase, deliver at once */

                cb(data, wp->opcode,
                   wp->ctrl ? wp->ctrl_buf : NULL, 0, 1);

                wp->state = NGX_LIVE_WS_ST_HEADER;
                wp->hdr_len = 0;
            }

            continue;
        }

        /* payload bytes of the current frame */

        n = (size_t) ngx_min((uint64_t) len, wp->left);

        if (wp->ctrl) {

            /* control payloads stay <= 125 bytes in total */

            if (wp->ctrl_len + n > NGX_LIVE_WS_LEN7_MAX) {
                return NGX_ERROR;
            }

            for (i = 0; i < n; i++) {
                wp->ctrl_buf[wp->ctrl_len + i] =
                    p[i] ^ wp->mask[wp->mask_off];
                wp->mask_off = (wp->mask_off + 1) & 3;
            }

            wp->ctrl_len += n;

        } else {
            ngx_live_ws_unmask(p, n, wp->mask, &wp->mask_off);
        }

        wp->left -= n;

        if (wp->left == 0) {

            payload = wp->ctrl ? wp->ctrl_buf : p;
            i = wp->ctrl ? wp->ctrl_len : n;

            cb(data, wp->opcode, payload, i, 1);

            wp->state = NGX_LIVE_WS_ST_HEADER;
            wp->hdr_len = 0;
            wp->mask_off = 0;
            wp->ctrl_len = 0;

        } else if (!wp->ctrl) {

            /*
             * data frames stream through frame boundaries: deliver
             * the fragment now, the rest continues in the next feed
             */

            cb(data, wp->opcode, p, n, 0);
        }

        p += n;
        len -= n;
    }

    return NGX_OK;
}


u_char *
ngx_live_ws_accept_key(const ngx_str_t *key, u_char *dst)
{
    u_char      buf[NGX_LIVE_WS_MAX_KEY + NGX_LIVE_WS_GUID_LEN];
    u_char      md[20];
    size_t      n;
    ngx_sha1_t  sha;
    ngx_str_t   digest, encoded;

    if (key->len == 0 || key->len > NGX_LIVE_WS_MAX_KEY) {
        return NULL;
    }

    n = key->len;

    ngx_memcpy(buf, key->data, n);
    ngx_memcpy(buf + n, NGX_LIVE_WS_GUID, NGX_LIVE_WS_GUID_LEN);
    n += NGX_LIVE_WS_GUID_LEN;

    ngx_sha1_init(&sha);
    ngx_sha1_update(&sha, buf, n);
    ngx_sha1_final(md, &sha);

    digest.data = md;
    digest.len = sizeof(md);

    encoded.data = dst;

    ngx_encode_base64(&encoded, &digest);    /* exactly 28 chars */

    dst[NGX_LIVE_WS_ACCEPT_LEN] = '\0';

    return dst;
}


size_t
ngx_live_ws_frame_header(u_char *buf, size_t len, ngx_uint_t opcode)
{
    size_t  i;

    buf[0] = (u_char) (0x80 | opcode);    /* FIN=1, servers never mask */

    if (len <= NGX_LIVE_WS_LEN7_MAX) {
        buf[1] = (u_char) len;
        return 2;
    }

    if (len <= 0xffff) {
        buf[1] = 126;
        buf[2] = (u_char) (len >> 8);
        buf[3] = (u_char) len;
        return 4;
    }

    buf[1] = 127;

    for (i = 0; i < 8; i++) {
        buf[2 + i] = (u_char) (len >> (56 - 8 * i));
    }

    return 10;
}


size_t
ngx_live_ws_control_frame(u_char *buf, size_t size, ngx_uint_t opcode,
    const u_char *payload, size_t len)
{
    size_t  n;

    if (len > NGX_LIVE_WS_LEN7_MAX) {
        return 0;
    }

    n = ngx_live_ws_frame_header(buf, len, opcode);

    if (size < n + len) {
        return 0;
    }

    if (len) {
        ngx_memcpy(buf + n, payload, len);
    }

    return n + len;
}
