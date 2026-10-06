
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"
#include "ngx_live_shared.h"
#include "ngx_live_stream.h"
#include "ngx_live_core_module.h"


static ngx_live_packet_t *ngx_live_stream_cache_packet(
    ngx_live_stream_t *stream, ngx_live_packet_t *pkt);
static void ngx_live_stream_free_packet(ngx_live_stream_t *stream,
    ngx_live_packet_t *cp);
static void ngx_live_gop_reset(ngx_live_stream_t *stream);


ngx_live_stream_t **
ngx_live_find_stream(ngx_live_core_app_conf_t *cacf, ngx_str_t *name,
    ngx_int_t create)
{
    ngx_live_stream_t  **stream;

    if (name->len >= NGX_LIVE_MAX_NAME) {
        return NULL;
    }

    stream = &cacf->streams[ngx_hash_key(name->data, name->len)
                            % NGX_LIVE_STREAM_BUCKETS];

    for (; *stream; stream = &(*stream)->next) {
        if (ngx_strncmp(name->data, (*stream)->name, name->len) == 0
            && (*stream)->name[name->len] == '\0')
        {
            return stream;
        }
    }

    if (!create) {
        return NULL;
    }

    if (cacf->free_streams) {
        *stream = cacf->free_streams;
        cacf->free_streams = cacf->free_streams->next;

    } else {
        *stream = ngx_palloc(cacf->pool, sizeof(ngx_live_stream_t));
        if (*stream == NULL) {
            return NULL;
        }
    }

    ngx_memzero(*stream, sizeof(ngx_live_stream_t));

    ngx_memcpy((*stream)->name, name->data, name->len);
    (*stream)->epoch = ngx_current_msec;
    (*stream)->app_conf = cacf;

    ngx_queue_init(&(*stream)->subscribers);
    ngx_queue_init(&(*stream)->gop_cache);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, cacf->log, 0,
                   "live: stream '%V' created in app '%V'",
                   name, &cacf->name);

    return stream;
}


ngx_int_t
ngx_live_stream_publish(ngx_live_stream_t *stream, void *publisher)
{
    if (stream->publishing) {
        return NGX_BUSY;
    }

    stream->publishing = 1;
    stream->publisher = publisher;
    stream->active = 0;

    return NGX_OK;
}


void
ngx_live_stream_close(ngx_live_stream_t *stream)
{
    ngx_queue_t           *item, *next;
    ngx_live_subscriber_t *sub;
    ngx_live_stream_t    **sp;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, stream->app_conf->log, 0,
                   "live: stream '%s' closing", stream->name);

    /*
     * Detach the whole subscriber list first, then notify: close
     * callbacks are allowed to touch their (already unlinked) nodes
     * and may finalize sessions synchronously.  The old chain still
     * terminates at the queue's own address, which stays valid.
     */

    item = ngx_queue_head(&stream->subscribers);

    ngx_queue_init(&stream->subscribers);
    stream->nsubscribers = 0;

    while (item != (ngx_queue_t *) &stream->subscribers) {
        next = ngx_queue_next(item);

        sub = ngx_queue_data(item, ngx_live_subscriber_t, queue);

        if (sub->close) {
            sub->close(sub->sess);
        }

        item = next;
    }

    /* drop cached packets */

    if (stream->meta) {
        ngx_live_stream_free_packet(stream, stream->meta);
        stream->meta = NULL;
    }

    if (stream->audio_header) {
        ngx_live_stream_free_packet(stream, stream->audio_header);
        stream->audio_header = NULL;
    }

    if (stream->video_header) {
        ngx_live_stream_free_packet(stream, stream->video_header);
        stream->video_header = NULL;
    }

    ngx_live_gop_reset(stream);

    /* unlink from hash, recycle */

    for (sp = &stream->app_conf->streams[ngx_hash_key(stream->name,
                                    ngx_strlen(stream->name))
                                    % NGX_LIVE_STREAM_BUCKETS];
         *sp;
         sp = &(*sp)->next)
    {
        if (*sp == stream) {
            *sp = stream->next;
            break;
        }
    }

    stream->publishing = 0;
    stream->publisher = NULL;
    stream->active = 0;

    stream->next = stream->app_conf->free_streams;
    stream->app_conf->free_streams = stream;
}


void
ngx_live_stream_packet(ngx_live_stream_t *stream, ngx_live_packet_t *pkt)
{
    ngx_queue_t           *item, *next;
    ngx_live_subscriber_t *sub;
    ngx_live_packet_t     *cp;

    if (!stream->active
        && (pkt->flags == NGX_LIVE_FLAG_AUDIO
            || pkt->flags == NGX_LIVE_FLAG_VIDEO_KEY
            || pkt->flags == NGX_LIVE_FLAG_VIDEO_INTER))
    {
        stream->active = 1;
    }

    if (pkt->flags != NGX_LIVE_FLAG_IGNORED) {

        stream->in_bytes += pkt->size + 15;    /* tag body + FLV framing */

        if (pkt->codec_type == NGX_LIVE_AUDIO) {
            stream->last_dts[0] = pkt->dts;
            stream->naudio++;

        } else if (pkt->codec_type == NGX_LIVE_VIDEO) {
            stream->last_dts[1] = pkt->dts;
            stream->nvideo++;
        }
    }

    /* update caches */

    switch (pkt->flags) {

    case NGX_LIVE_FLAG_AUDIO_HEADER:
        cp = ngx_live_stream_cache_packet(stream, pkt);
        if (cp) {
            if (stream->audio_header) {
                ngx_live_stream_free_packet(stream, stream->audio_header);
            }
            stream->audio_header = cp;
        }
        break;

    case NGX_LIVE_FLAG_VIDEO_HEADER:
        cp = ngx_live_stream_cache_packet(stream, pkt);
        if (cp) {
            if (stream->video_header) {
                ngx_live_stream_free_packet(stream, stream->video_header);
            }
            stream->video_header = cp;
        }
        break;

    case NGX_LIVE_FLAG_META:
        cp = ngx_live_stream_cache_packet(stream, pkt);
        if (cp) {
            if (stream->meta) {
                ngx_live_stream_free_packet(stream, stream->meta);
            }
            stream->meta = cp;
        }
        break;

    default:
        if (stream->app_conf->gop_cache) {

            if (pkt->flags == NGX_LIVE_FLAG_VIDEO_KEY) {
                ngx_live_gop_reset(stream);

            } else if (stream->gop_count >= NGX_LIVE_GOP_MAX)
            {
                /*
                 * Overflow guard for streams without keyframes
                 * (open-GOP/CRA sources): drop the cache.  Caching
                 * restarts from the next packet; such streams have
                 * no random access point anyway.
                 */

                ngx_live_gop_reset(stream);
            }

            if (pkt->flags == NGX_LIVE_FLAG_AUDIO
                || pkt->flags == NGX_LIVE_FLAG_VIDEO_KEY
                || pkt->flags == NGX_LIVE_FLAG_VIDEO_INTER)
            {
                cp = ngx_live_stream_cache_packet(stream, pkt);
                if (cp) {
                    ngx_live_gop_node_t  *node;

                    node = ngx_queue_data(cp, ngx_live_gop_node_t, pkt);
                    ngx_queue_insert_tail(&stream->gop_cache, &node->queue);
                    stream->gop_count++;
                }
            }
        }
    }

    /* fan out */

    item = ngx_queue_head(&stream->subscribers);

    while (item != ngx_queue_sentinel(&stream->subscribers)) {

        next = ngx_queue_next(item);

        sub = ngx_queue_data(item, ngx_live_subscriber_t, queue);

        if (sub->send) {
            if (sub->send(sub->sess, pkt) != NGX_OK) {
                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, stream->app_conf->log, 0,
                               "live: subscriber send failed on '%s'",
                               stream->name);
            }
        }

        item = next;
    }
}


ngx_int_t
ngx_live_stream_subscribe(ngx_live_stream_t *stream,
    ngx_live_subscriber_t *sub)
{
    ngx_queue_insert_tail(&stream->subscribers, &sub->queue);
    stream->nsubscribers++;

    return NGX_OK;
}


void
ngx_live_stream_unsubscribe(ngx_live_stream_t *stream,
    ngx_live_subscriber_t *sub)
{
    ngx_queue_remove(&sub->queue);
    stream->nsubscribers--;
}


static ngx_live_packet_t *
ngx_live_stream_cache_packet(ngx_live_stream_t *stream,
    ngx_live_packet_t *pkt)
{
    ngx_live_gop_node_t  *node;

    node = ngx_palloc(stream->app_conf->pool, sizeof(ngx_live_gop_node_t));
    if (node == NULL) {
        return NULL;
    }

    node->pkt = *pkt;
    ngx_queue_init(&node->queue);

    ngx_live_acquire_shared_chain(pkt->chain);

    return &node->pkt;
}


static void
ngx_live_stream_free_packet(ngx_live_stream_t *stream,
    ngx_live_packet_t *cp)
{
    ngx_live_gop_node_t  *node;

    ngx_live_free_shared_chain(stream->app_conf, cp->chain);

    node = ngx_queue_data(cp, ngx_live_gop_node_t, pkt);

    /* the caller has already unlinked gop nodes from the queue */

    ngx_pfree(stream->app_conf->pool, node);
}


static void
ngx_live_gop_reset(ngx_live_stream_t *stream)
{
    ngx_queue_t          *item;
    ngx_live_gop_node_t  *node;

    while (!ngx_queue_empty(&stream->gop_cache)) {
        item = ngx_queue_head(&stream->gop_cache);
        node = ngx_queue_data(item, ngx_live_gop_node_t, queue);

        ngx_queue_remove(item);
        ngx_live_free_shared_chain(stream->app_conf, node->pkt.chain);

        ngx_pfree(stream->app_conf->pool, node);
    }

    stream->gop_count = 0;
}
