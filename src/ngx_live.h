
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_H_INCLUDED_
#define _NGX_LIVE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <nginx.h>


typedef struct {
    void                     **main_conf;
    void                     **srv_conf;
    void                     **app_conf;
} ngx_live_conf_ctx_t;


typedef struct {
    ngx_int_t               (*preconfiguration)(ngx_conf_t *cf);
    ngx_int_t               (*postconfiguration)(ngx_conf_t *cf);

    void                    *(*create_main_conf)(ngx_conf_t *cf);
    char                    *(*init_main_conf)(ngx_conf_t *cf, void *conf);

    void                    *(*create_srv_conf)(ngx_conf_t *cf);
    char                    *(*merge_srv_conf)(ngx_conf_t *cf, void *prev,
                                    void *conf);

    void                    *(*create_app_conf)(ngx_conf_t *cf);
    char                    *(*merge_app_conf)(ngx_conf_t *cf, void *prev,
                                    void *conf);
} ngx_live_module_t;


#define NGX_LIVE_MODULE                 0x4C495645     /* "LIVE" */
#define NGX_LIVE_MODULE_VERSION         "2.0.0"
#define ngx_live_module_version         2000000
/*
 * Context flags, one per nesting level of the rtmp{} block
 * (rtmp{} -> server{} -> application{}), after ngx_rtmp_*.
 */

#define NGX_LIVE_MAIN_CONF              0x02000000
#define NGX_LIVE_SRV_CONF               0x04000000
#define NGX_LIVE_APP_CONF               0x08000000


#define NGX_LIVE_MAIN_CONF_OFFSET  offsetof(ngx_live_conf_ctx_t, main_conf)
#define NGX_LIVE_SRV_CONF_OFFSET   offsetof(ngx_live_conf_ctx_t, srv_conf)
#define NGX_LIVE_APP_CONF_OFFSET   offsetof(ngx_live_conf_ctx_t, app_conf)


#define ngx_live_conf_get_module_main_conf(cf, module)                       \
    ((ngx_live_conf_ctx_t *) cf->ctx)->main_conf[module.ctx_index]

#define ngx_live_conf_get_module_srv_conf(cf, module)                        \
    ((ngx_live_conf_ctx_t *) cf->ctx)->srv_conf[module.ctx_index]

#define ngx_live_conf_get_module_app_conf(cf, module)                        \
    ((ngx_live_conf_ctx_t *) cf->ctx)->app_conf[module.ctx_index]


extern ngx_uint_t   ngx_live_max_module;
extern ngx_module_t ngx_live_module;


/* forward declarations (defined in respective headers) */

typedef struct ngx_live_core_app_conf_s   ngx_live_core_app_conf_t;
typedef struct ngx_live_stream_s          ngx_live_stream_t;
typedef struct ngx_live_subscriber_s      ngx_live_subscriber_t;


/* packet codec_type, same as FLV TagType */

#define NGX_LIVE_AUDIO              8
#define NGX_LIVE_VIDEO              9
#define NGX_LIVE_SCRIPT             18


/* packet flags: how the stream core treats a packet */

#define NGX_LIVE_FLAG_AUDIO_HEADER  0   /* audio sequence header */
#define NGX_LIVE_FLAG_AUDIO         1
#define NGX_LIVE_FLAG_VIDEO_HEADER  2   /* video sequence header */
#define NGX_LIVE_FLAG_VIDEO_KEY     3   /* video keyframe: resets GOP cache */
#define NGX_LIVE_FLAG_VIDEO_INTER   4
#define NGX_LIVE_FLAG_META          5   /* script data (onMetaData) */
#define NGX_LIVE_FLAG_IGNORED       6   /* Enhanced RTMP video Metadata /
                                           SequenceEnd (video/audio): passed
                                           through, never cached */


/*
 * codec_id: legacy FLV CodecID/SoundFormat value (small integer),
 * or the Enhanced RTMP FourCC read as uint32 big-endian.  The two
 * ranges never collide (FourCC bytes are ASCII >= 0x2e).
 */

#define NGX_LIVE_FOURCC_AV1         0x61763031U    /* "av01" */
#define NGX_LIVE_FOURCC_VP9         0x76703039U    /* "vp09" */
#define NGX_LIVE_FOURCC_HEVC        0x68766331U    /* "hvc1" */

#define NGX_LIVE_FOURCC_AC3         0x61632d33U    /* "ac-3" */
#define NGX_LIVE_FOURCC_EAC3        0x65632d33U    /* "ec-3" */
#define NGX_LIVE_FOURCC_OPUS        0x4f707573U    /* "Opus" */
#define NGX_LIVE_FOURCC_MP3         0x2e6d7033U    /* ".mp3" */
#define NGX_LIVE_FOURCC_FLAC        0x664c6143U    /* "fLaC" */

#define NGX_LIVE_CODECID_AAC        10
#define NGX_LIVE_CODECID_AVC        7

/* SoundFormat 9 marks an Enhanced RTMP audio tag */
#define NGX_LIVE_SOUNDFORMAT_EX     9


#endif /* _NGX_LIVE_H_INCLUDED_ */
