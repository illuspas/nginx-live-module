
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_CORE_MODULE_H_INCLUDED_
#define _NGX_LIVE_CORE_MODULE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"


typedef struct ngx_live_core_app_conf_s {

    /* application name, e.g. "myapp" */
    ngx_str_t               name;

    /* array of ngx_live_module_t app_confs for this application */
    void                  **app_conf;

    /* directives */

    ngx_flag_t              live;            /* live on|off */
    ngx_flag_t              gop_cache;       /* gop_cache on|off */

    /*
     * Runtime (initialized at merge time).  The pool owns stream
     * structures, shared buffers and GOP nodes; it outlives
     * individual streams and sessions, matching per-worker
     * semantics.
     */
    ngx_pool_t             *pool;
    ngx_log_t              *log;
    ngx_chain_t            *free_bufs;     /* recycled shared buffers */
    ngx_live_stream_t     **streams;          /* hash buckets */
    ngx_live_stream_t      *free_streams;     /* recycled streams */

}
ngx_live_core_app_conf_t;


/* one rtmp{} server{} conf */

typedef struct ngx_live_core_srv_conf_s {

    /* the server{} conf context owning this srv conf */
    ngx_live_conf_ctx_t    *ctx;

    /* array of ngx_live_core_app_conf_t* defined in this server{} */
    ngx_array_t             applications;
} ngx_live_core_srv_conf_t;


/* one listen entry of the rtmp{} block (RTMP transport) */

typedef struct {
    u_char                  sockaddr[NGX_SOCKADDRLEN];
    socklen_t               socklen;

    /* the server{} conf context of the listen directive */
    ngx_live_conf_ctx_t    *ctx;

    unsigned                wildcard:1;
    unsigned                ipv6only:1;
} ngx_live_listen_t;


typedef struct {

    /* array of ngx_live_core_srv_conf_t*, in configuration order */
    ngx_array_t             servers;

    /* array of ngx_live_core_app_conf_t*, all servers, for runtime lookup */
    ngx_array_t             applications;

    /* array of ngx_live_listen_t */
    ngx_array_t             listening;

} ngx_live_core_main_conf_t;


/* global main conf, for HTTP-side access (live_flv handler) */
extern ngx_live_core_main_conf_t *ngx_live_core_main_conf;

extern ngx_module_t ngx_live_core_module;


#endif /* _NGX_LIVE_CORE_MODULE_H_INCLUDED_ */
