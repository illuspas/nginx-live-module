
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"
#include "ngx_live_packet.h"


size_t
ngx_live_chain_read(ngx_chain_t *in, u_char *dst, size_t n)
{
    u_char  *p, *last;
    size_t   len, total;

    total = 0;

    while (in && n) {
        p = in->buf->pos;
        last = in->buf->last;
        len = ngx_min((size_t) (last - p), n);

        ngx_memcpy(dst, p, len);
        dst += len;
        total += len;
        n -= len;

        in = in->next;
    }

    return total;
}


/*
 * Enhanced RTMP v1 video/audio packet types (low 4 bits of byte0
 * when the Enhanced marker is set).
 */

#define NGX_LIVE_EX_SEQ_START       0   /* SequenceStart: codec config */
#define NGX_LIVE_EX_CODED_FRAMES    1   /* CodedFrames: cts only for hvc1 */
#define NGX_LIVE_EX_SEQ_END         2   /* SequenceEnd: ignored */
#define NGX_LIVE_EX_CODED_FRAMES_X  3   /* CodedFramesX: never has cts */
#define NGX_LIVE_EX_METADATA        4   /* Metadata (hdrMetadata): ignored */


static uint32_t
ngx_live_read_fourcc(u_char *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
           | ((uint32_t) p[2] << 8) | p[3];
}


ngx_int_t
ngx_live_packet_identify(ngx_live_packet_t *pkt)
{
    u_char     b[8];
    size_t     n;
    ngx_uint_t frame_type, codec, packet_type;

    switch (pkt->codec_type) {

    case NGX_LIVE_SCRIPT:
        pkt->flags = NGX_LIVE_FLAG_META;
        return NGX_OK;

    case NGX_LIVE_AUDIO:
        n = ngx_live_chain_read(pkt->chain, b, 5);
        if (n < 1) {
            return NGX_ERROR;
        }

        codec = b[0] >> 4;

        /*
         * AudioTagHeader byte0:
         *   bit7..4  SoundFormat
         *   bit3..0  AudioPacketType when SoundFormat == 9
         */

        if (codec == NGX_LIVE_SOUNDFORMAT_EX) {
            if (n < 5) {
                return NGX_ERROR;
            }

            pkt->codec_id = ngx_live_read_fourcc(&b[1]);

            switch (b[0] & 0x0f) {

            case NGX_LIVE_EX_SEQ_START:
                pkt->flags = NGX_LIVE_FLAG_AUDIO_HEADER;
                break;

            case NGX_LIVE_EX_CODED_FRAMES:
                pkt->flags = NGX_LIVE_FLAG_AUDIO;
                break;

            default:
                /* SequenceEnd, multichannel/multitrack: pass through */
                pkt->flags = NGX_LIVE_FLAG_IGNORED;
                break;
            }

            return NGX_OK;
        }

        if (codec == NGX_LIVE_CODECID_AAC) {
            if (n < 2) {
                return NGX_ERROR;
            }
            pkt->flags = (b[1] == 0) ? NGX_LIVE_FLAG_AUDIO_HEADER
                                     : NGX_LIVE_FLAG_AUDIO;

        } else {
            pkt->flags = NGX_LIVE_FLAG_AUDIO;
        }

        pkt->codec_id = codec;
        return NGX_OK;

    case NGX_LIVE_VIDEO:
        n = ngx_live_chain_read(pkt->chain, b, 8);
        if (n < 1) {
            return NGX_ERROR;
        }

        /*
         * VideoTagHeader byte0:
         *   bit7     IsExHeader (Enhanced RTMP: FourCC follows)
         *   bit6..4  FrameType (1 = keyframe)
         *   bit3..0  CodecID, or VideoPacketType when enhanced
         */

        frame_type = (b[0] >> 4) & 7;

        if (b[0] & 0x80) {
            if (n < 5) {
                return NGX_ERROR;
            }

            pkt->codec_id = ngx_live_read_fourcc(&b[1]);
            packet_type = b[0] & 0x0f;

            switch (packet_type) {

            case NGX_LIVE_EX_SEQ_START:
                pkt->flags = NGX_LIVE_FLAG_VIDEO_HEADER;
                break;

            case NGX_LIVE_EX_CODED_FRAMES:
            case NGX_LIVE_EX_CODED_FRAMES_X:
                pkt->flags = (frame_type == 1) ? NGX_LIVE_FLAG_VIDEO_KEY
                                               : NGX_LIVE_FLAG_VIDEO_INTER;

                /*
                 * Only HEVC (hvc1) CodedFrames carry a composition
                 * time offset; AV1/VP9 and CodedFramesX never do.
                 */

                if (packet_type == NGX_LIVE_EX_CODED_FRAMES
                    && pkt->codec_id == NGX_LIVE_FOURCC_HEVC
                    && n >= 8)
                {
                    pkt->cts = ((int32_t) b[5] << 16)
                               | ((int32_t) b[6] << 8) | b[7];
                }
                break;

            default:
                /* SequenceEnd, Metadata: pass through */
                pkt->flags = NGX_LIVE_FLAG_IGNORED;
                break;
            }

            return NGX_OK;
        }

        codec = b[0] & 0x0f;

        if (codec == NGX_LIVE_CODECID_AVC) {
            if (n < 5) {
                return NGX_ERROR;
            }

            if (b[1] == 0) {
                pkt->flags = NGX_LIVE_FLAG_VIDEO_HEADER;

            } else {
                pkt->flags = (frame_type == 1) ? NGX_LIVE_FLAG_VIDEO_KEY
                                               : NGX_LIVE_FLAG_VIDEO_INTER;
            }

            /* composition time, 3 bytes big-endian */
            pkt->cts = ((int32_t) b[2] << 16) | ((int32_t) b[3] << 8) | b[4];

        } else {
            pkt->flags = (frame_type == 1) ? NGX_LIVE_FLAG_VIDEO_KEY
                                           : NGX_LIVE_FLAG_VIDEO_INTER;
        }

        pkt->codec_id = codec;
        return NGX_OK;

    default:
        return NGX_ERROR;
    }
}


/*
 * Human readable codec name for stats/logging.  At least 8 bytes
 * of storage must be pointed to by buf.
 */

u_char *
ngx_live_codec_name(ngx_uint_t codec_type, uint32_t codec_id, u_char *buf)
{
    u_char  *name;

    if (codec_id > 0xffff) {
        buf[0] = (u_char) (codec_id >> 24);
        buf[1] = (u_char) (codec_id >> 16);
        buf[2] = (u_char) (codec_id >> 8);
        buf[3] = (u_char) codec_id;
        buf[4] = '\0';
        return buf;
    }

    name = (u_char *) "";

    if (codec_type == NGX_LIVE_VIDEO) {
        switch (codec_id) {
        case 2:  name = (u_char *) "H263";   break;
        case 3:  name = (u_char *) "Screen"; break;
        case 4:  name = (u_char *) "VP6";    break;
        case 5:  name = (u_char *) "VP6A";   break;
        case 6:  name = (u_char *) "Screen2";break;
        case NGX_LIVE_CODECID_AVC: name = (u_char *) "H264"; break;
        case 8:  name = (u_char *) "H263R";  break;
        case 9:  name = (u_char *) "MPEG4";  break;
        }

    } else if (codec_type == NGX_LIVE_AUDIO) {
        switch (codec_id) {
        case 0:  name = (u_char *) "PCM";    break;
        case 1:  name = (u_char *) "ADPCM";  break;
        case 2:  name = (u_char *) "MP3";    break;
        case 3:  name = (u_char *) "PCM_LE"; break;
        case 4:  name = (u_char *) "Nelly16k"; break;
        case 5:  name = (u_char *) "Nelly8k";  break;
        case 6:  name = (u_char *) "Nelly";  break;
        case 7:  name = (u_char *) "PCM_ALAW"; break;
        case 8:  name = (u_char *) "PCM_ULAW"; break;
        case NGX_LIVE_CODECID_AAC: name = (u_char *) "AAC"; break;
        case 11: name = (u_char *) "Speex";  break;
        }
    }

    if (name[0] == '\0') {
        ngx_sprintf(buf, "%uxd", codec_id);
        return buf;
    }

    ngx_memcpy(buf, name, ngx_strlen(name) + 1);
    return buf;
}


void
ngx_live_packet_prepend_tag_header(ngx_live_packet_t *pkt)
{
    u_char       *p;
    ngx_chain_t  *cl;

    /* the tag body must start in the first buffer, inside headroom */

    cl = pkt->chain;

    p = cl->buf->pos - 11;

    *p++ = (u_char) pkt->codec_type;

    *p++ = (u_char) (pkt->size >> 16);
    *p++ = (u_char) (pkt->size >> 8);
    *p++ = (u_char) pkt->size;

    *p++ = (u_char) (pkt->dts >> 16);
    *p++ = (u_char) (pkt->dts >> 8);
    *p++ = (u_char) pkt->dts;
    *p++ = (u_char) (pkt->dts >> 24);   /* timestamp extended */

    *p++ = 0;    /* stream id, always 0 */
    *p++ = 0;
    *p++ = 0;

    cl->buf->pos = p - 11;
}
