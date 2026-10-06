
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <nginx.h>
#include "ngx_live.h"
#include "ngx_live_stream.h"
#include "ngx_live_core_module.h"


static void *ngx_live_core_create_main_conf(ngx_conf_t *cf);
static void *ngx_live_core_create_srv_conf(ngx_conf_t *cf);
static void *ngx_live_core_create_app_conf(ngx_conf_t *cf);
static char *ngx_live_core_merge_app_conf(ngx_conf_t *cf, void *prev,
    void *conf);
static char *ngx_live_core_server(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_live_core_application(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_live_core_listen(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


ngx_live_core_main_conf_t *ngx_live_core_main_conf = NULL;


static ngx_command_t  ngx_live_core_commands[] = {

    { ngx_string("server"),
      NGX_LIVE_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_live_core_server,
      0,
      0,
      NULL },

    { ngx_string("listen"),
      NGX_LIVE_SRV_CONF|NGX_CONF_TAKE12,
      ngx_live_core_listen,
      NGX_LIVE_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("application"),
      NGX_LIVE_SRV_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE1,
      ngx_live_core_application,
      NGX_LIVE_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("live"),
      NGX_LIVE_MAIN_CONF|NGX_LIVE_SRV_CONF|NGX_LIVE_APP_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_LIVE_APP_CONF_OFFSET,
      offsetof(ngx_live_core_app_conf_t, live),
      NULL },

    { ngx_string("gop_cache"),
      NGX_LIVE_MAIN_CONF|NGX_LIVE_SRV_CONF|NGX_LIVE_APP_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_LIVE_APP_CONF_OFFSET,
      offsetof(ngx_live_core_app_conf_t, gop_cache),
      NULL },

      ngx_null_command
};


static ngx_live_module_t  ngx_live_core_module_ctx = {
    NULL,                                   /* preconfiguration */
    NULL,                                   /* postconfiguration */

    ngx_live_core_create_main_conf,         /* create main configuration */
    NULL,                                   /* init main configuration */

    ngx_live_core_create_srv_conf,          /* create server configuration */
    NULL,                                   /* merge server configuration */

    ngx_live_core_create_app_conf,          /* create app configuration */
    ngx_live_core_merge_app_conf            /* merge app configuration */
};


ngx_module_t  ngx_live_core_module = {
    NGX_MODULE_V1,
    &ngx_live_core_module_ctx,          /* module context */
    ngx_live_core_commands,             /* module directives */
    NGX_LIVE_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_live_core_create_main_conf(ngx_conf_t *cf)
{
    ngx_live_core_main_conf_t  *cmcf;

    cmcf = ngx_pcalloc(cf->pool, sizeof(ngx_live_core_main_conf_t));
    if (cmcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&cmcf->servers, cf->pool, 1,
                       sizeof(ngx_live_core_srv_conf_t *))
        != NGX_OK)
    {
        return NULL;
    }

    if (ngx_array_init(&cmcf->applications, cf->pool, 4,
                       sizeof(ngx_live_core_app_conf_t *))
        != NGX_OK)
    {
        return NULL;
    }

    if (ngx_array_init(&cmcf->listening, cf->pool, 4,
                       sizeof(ngx_live_listen_t))
        != NGX_OK)
    {
        return NULL;
    }

    ngx_live_core_main_conf = cmcf;

    return cmcf;
}


static void *
ngx_live_core_create_srv_conf(ngx_conf_t *cf)
{
    ngx_live_core_srv_conf_t  *cscf;

    cscf = ngx_pcalloc(cf->pool, sizeof(ngx_live_core_srv_conf_t));
    if (cscf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&cscf->applications, cf->pool, 4,
                       sizeof(ngx_live_core_app_conf_t *))
        != NGX_OK)
    {
        return NULL;
    }

    return cscf;
}


static void *
ngx_live_core_create_app_conf(ngx_conf_t *cf)
{
    ngx_live_core_app_conf_t  *cacf;

    cacf = ngx_pcalloc(cf->pool, sizeof(ngx_live_core_app_conf_t));
    if (cacf == NULL) {
        return NULL;
    }

    cacf->live = NGX_CONF_UNSET;
    cacf->gop_cache = NGX_CONF_UNSET;

    return cacf;
}


static char *
ngx_live_core_merge_app_conf(ngx_conf_t *cf, void *parent, void *conf)
{
    ngx_live_core_app_conf_t *prev = parent;
    ngx_live_core_app_conf_t *cacf = conf;

    ngx_conf_merge_value(cacf->live, prev->live, 0);
    ngx_conf_merge_value(cacf->gop_cache, prev->gop_cache, 1);

    /*
     * The rtmp{}/server{} level app_confs are merge scaffolding only
     * (cacf->app_conf is set by the application{} handler); runtime
     * state belongs to real applications alone.
     */

    if (cacf->live == 0 || cacf->app_conf == NULL) {
        return NGX_CONF_OK;
    }

    /* runtime init: stream pool, hash buckets */

    cacf->pool = ngx_create_pool(4096, &cf->cycle->new_log);
    if (cacf->pool == NULL) {
        return NGX_CONF_ERROR;
    }

    cacf->log = &cf->cycle->new_log;

    cacf->streams = ngx_pcalloc(cacf->pool,
                                sizeof(ngx_live_stream_t *)
                                * NGX_LIVE_STREAM_BUCKETS);
    if (cacf->streams == NULL) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static char *
ngx_live_core_server(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                       *rv;
    ngx_uint_t                  m;
    ngx_conf_t                  pcf;
    ngx_module_t              **modules;
    ngx_live_module_t          *module;
    ngx_live_conf_ctx_t        *ctx, *pctx;
    ngx_live_core_srv_conf_t   *cscf, **cscfp;
    ngx_live_core_main_conf_t  *cmcf;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_live_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    pctx = cf->ctx;
    ctx->main_conf = pctx->main_conf;

    /* the server{}'s srv_conf */

    ctx->srv_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_live_max_module);
    if (ctx->srv_conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * the server{}'s null app_conf context, it is used to merge
     * the applications{}' app_conf's of this server
     */

    ctx->app_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_live_max_module);
    if (ctx->app_conf == NULL) {
        return NGX_CONF_ERROR;
    }

    modules = cf->cycle->modules;

    for (m = 0; modules[m]; m++) {
        if (modules[m]->type != NGX_LIVE_MODULE) {
            continue;
        }

        module = modules[m]->ctx;

        if (module->create_srv_conf) {
            ctx->srv_conf[modules[m]->ctx_index] = module->create_srv_conf(cf);
            if (ctx->srv_conf[modules[m]->ctx_index] == NULL) {
                return NGX_CONF_ERROR;
            }
        }

        if (module->create_app_conf) {
            ctx->app_conf[modules[m]->ctx_index] = module->create_app_conf(cf);
            if (ctx->app_conf[modules[m]->ctx_index] == NULL) {
                return NGX_CONF_ERROR;
            }
        }
    }

    cscf = ctx->srv_conf[ngx_live_core_module.ctx_index];
    cscf->ctx = ctx;

    cmcf = ctx->main_conf[ngx_live_core_module.ctx_index];

    cscfp = ngx_array_push(&cmcf->servers);
    if (cscfp == NULL) {
        return NGX_CONF_ERROR;
    }

    *cscfp = cscf;

    /* parse inside server{} */

    pcf = *cf;
    cf->ctx = ctx;
    cf->cmd_type = NGX_LIVE_SRV_CONF;

    rv = ngx_conf_parse(cf, NULL);

    *cf = pcf;

    return rv;
}


static char *
ngx_live_core_application(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                       *rv;
    ngx_uint_t                  i;
    ngx_str_t                  *value;
    ngx_conf_t                  save;
    ngx_module_t             **modules;
    ngx_live_module_t          *module;
    ngx_live_conf_ctx_t        *ctx, *pctx;
    ngx_live_core_srv_conf_t   *cscf;
    ngx_live_core_app_conf_t   *cacf, **cacfp;
    ngx_live_core_main_conf_t  *cmcf;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_live_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    pctx = cf->ctx;
    ctx->main_conf = pctx->main_conf;
    ctx->srv_conf = pctx->srv_conf;

    ctx->app_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_live_max_module);
    if (ctx->app_conf == NULL) {
        return NGX_CONF_ERROR;
    }

    modules = cf->cycle->modules;

    for (i = 0; modules[i]; i++) {
        if (modules[i]->type != NGX_LIVE_MODULE) {
            continue;
        }

        module = modules[i]->ctx;

        if (module->create_app_conf) {
            ctx->app_conf[modules[i]->ctx_index] = module->create_app_conf(cf);
            if (ctx->app_conf[modules[i]->ctx_index] == NULL) {
                return NGX_CONF_ERROR;
            }
        }
    }

    cacf = ctx->app_conf[ngx_live_core_module.ctx_index];
    cacf->app_conf = ctx->app_conf;

    value = cf->args->elts;

    cacf->name = value[1];

    /* register in the owning server{} (merge scope) */

    cscf = pctx->srv_conf[ngx_live_core_module.ctx_index];

    cacfp = ngx_array_push(&cscf->applications);
    if (cacfp == NULL) {
        return NGX_CONF_ERROR;
    }

    *cacfp = cacf;

    /* and globally, for runtime lookup by name (RTMP/HTTP-FLV/stat) */

    cmcf = pctx->main_conf[ngx_live_core_module.ctx_index];

    cacfp = ngx_array_push(&cmcf->applications);
    if (cacfp == NULL) {
        return NGX_CONF_ERROR;
    }

    *cacfp = cacf;

    save = *cf;
    cf->ctx = ctx;
    cf->cmd_type = NGX_LIVE_APP_CONF;

    rv = ngx_conf_parse(cf, NULL);

    *cf = save;

    return rv;
}


static char *
ngx_live_core_listen(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    size_t                      len, off;
    in_port_t                   port;
    ngx_str_t                  *value;
    ngx_url_t                   u;
    ngx_uint_t                  i;
    struct sockaddr            *sa;
    ngx_live_listen_t          *ls;
    struct sockaddr_in         *sin;
    ngx_live_core_main_conf_t  *cmcf;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6        *sin6;
#endif

    value = cf->args->elts;

    ngx_memzero(&u, sizeof(ngx_url_t));

    u.url = value[1];
    u.listen = 1;

    if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "%s in \"%V\" of the \"listen\" directive",
                               u.err, &u.url);
        }

        return NGX_CONF_ERROR;
    }

    cmcf = ngx_live_conf_get_module_main_conf(cf, ngx_live_core_module);

    /* reject duplicate address:port pairs */

    ls = cmcf->listening.elts;

    for (i = 0; i < cmcf->listening.nelts; i++) {

        sa = (struct sockaddr *) ls[i].sockaddr;

        if (sa->sa_family != u.family) {
            continue;
        }

        switch (sa->sa_family) {

#if (NGX_HAVE_INET6)
        case AF_INET6:
            off = offsetof(struct sockaddr_in6, sin6_addr);
            len = 16;
            sin6 = (struct sockaddr_in6 *) sa;
            port = sin6->sin6_port;
            break;
#endif

        default: /* AF_INET */
            off = offsetof(struct sockaddr_in, sin_addr);
            len = 4;
            sin = (struct sockaddr_in *) sa;
            port = sin->sin_port;
            break;
        }

        if (ngx_memcmp((u_char *) ls[i].sockaddr + off,
                       (u_char *) &u.sockaddr + off, len) != 0)
        {
            continue;
        }

        if (port != u.port) {
            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "duplicate \"%V\" address and port pair", &u.url);
        return NGX_CONF_ERROR;
    }

    ls = ngx_array_push(&cmcf->listening);
    if (ls == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memzero(ls, sizeof(ngx_live_listen_t));

    ngx_memcpy(ls->sockaddr, (u_char *) &u.sockaddr, u.socklen);

    ls->socklen = u.socklen;
    ls->wildcard = u.wildcard;
    ls->ctx = cf->ctx;

    /* optional parameter, as in nginx-rtmp: bind | ipv6only=on|off */

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strcmp(value[i].data, "bind") == 0) {
            /* the listening socket is always bound explicitly */
            continue;
        }

        if (ngx_strncmp(value[i].data, "ipv6only=", 9) == 0) {

            if (ngx_strcmp(&value[i].data[9], "on") == 0) {
                ls->ipv6only = 1;
                continue;
            }

            if (ngx_strcmp(&value[i].data[9], "off") == 0) {
                ls->ipv6only = 0;
                continue;
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid ipv6only value \"%s\"",
                               &value[i].data[9]);
            return NGX_CONF_ERROR;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid parameter \"%V\"", &value[i]);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}
