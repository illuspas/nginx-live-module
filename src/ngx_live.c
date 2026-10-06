
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <nginx.h>
#include "ngx_live.h"
#include "ngx_live_core_module.h"
#include "ngx_live_rtmp.h"


static char *ngx_live_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_live_merge_applications(ngx_conf_t *cf,
    ngx_array_t *applications, void **app_conf, ngx_live_module_t *module,
    ngx_uint_t ctx_index);
static ngx_int_t ngx_live_open_listening(ngx_conf_t *cf,
    ngx_live_core_main_conf_t *cmcf);


ngx_uint_t  ngx_live_max_module;


static ngx_command_t  ngx_live_commands[] = {

    { ngx_string("rtmp"),
      NGX_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_NOARGS,
      ngx_live_block,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_live_module_ctx = {
    ngx_string("rtmp"),
    NULL,
    NULL
};


ngx_module_t  ngx_live_module = {
    NGX_MODULE_V1,
    &ngx_live_module_ctx,               /* module context */
    ngx_live_commands,                  /* module directives */
    NGX_CORE_MODULE,                    /* module type */
    NULL,                               /* init master */
    NULL,                               /* init module */
    NULL,                               /* init process */
    NULL,                               /* init thread */
    NULL,                               /* exit thread */
    NULL,                               /* exit process */
    NULL,                               /* exit master */
    NGX_MODULE_V1_PADDING
};


static char *
ngx_live_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char                         *rv;
    ngx_uint_t                    m, mi, s;
    ngx_conf_t                    pcf;
    ngx_module_t                **modules;
    ngx_live_module_t            *module;
    ngx_live_conf_ctx_t          *ctx;
    ngx_live_core_srv_conf_t    **cscfp;
    ngx_live_core_main_conf_t    *cmcf;

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_live_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    *(ngx_live_conf_ctx_t **) conf = ctx;

    /* count the number of the live modules and set up their indices */

    ngx_live_max_module = ngx_count_modules(cf->cycle, NGX_LIVE_MODULE);

    /* the rtmp{} main_conf context, shared by all live contexts */

    ctx->main_conf = ngx_pcalloc(cf->pool,
                                 sizeof(void *) * ngx_live_max_module);
    if (ctx->main_conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * the rtmp{} null srv_conf context, it is used to merge
     * the server{}s' srv_conf's
     */

    ctx->srv_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_live_max_module);
    if (ctx->srv_conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * the rtmp{} null app_conf context, it is used to merge
     * the server{}s' app_conf's
     */

    ctx->app_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_live_max_module);
    if (ctx->app_conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * create the main_conf's, the null srv_conf's and the null app_conf's
     * of all live modules
     */

    modules = cf->cycle->modules;

    for (m = 0; modules[m]; m++) {
        if (modules[m]->type != NGX_LIVE_MODULE) {
            continue;
        }

        module = modules[m]->ctx;
        mi = modules[m]->ctx_index;

        if (module->create_main_conf) {
            ctx->main_conf[mi] = module->create_main_conf(cf);
            if (ctx->main_conf[mi] == NULL) {
                return NGX_CONF_ERROR;
            }
        }

        if (module->create_srv_conf) {
            ctx->srv_conf[mi] = module->create_srv_conf(cf);
            if (ctx->srv_conf[mi] == NULL) {
                return NGX_CONF_ERROR;
            }
        }

        if (module->create_app_conf) {
            ctx->app_conf[mi] = module->create_app_conf(cf);
            if (ctx->app_conf[mi] == NULL) {
                return NGX_CONF_ERROR;
            }
        }
    }

    pcf = *cf;
    cf->ctx = ctx;

    for (m = 0; modules[m]; m++) {
        if (modules[m]->type != NGX_LIVE_MODULE) {
            continue;
        }

        module = modules[m]->ctx;

        if (module->preconfiguration) {
            if (module->preconfiguration(cf) != NGX_OK) {
                *cf = pcf;
                return NGX_CONF_ERROR;
            }
        }
    }

    /* parse inside the rtmp{} block */

    cf->module_type = NGX_LIVE_MODULE;
    cf->cmd_type = NGX_LIVE_MAIN_CONF;
    rv = ngx_conf_parse(cf, NULL);

    if (rv != NGX_CONF_OK) {
        *cf = pcf;
        return rv;
    }

    /*
     * init rtmp{} main_conf's, then for every server{} merge
     * its srv_conf's and its applications' app_conf's
     */

    cmcf = ctx->main_conf[ngx_live_core_module.ctx_index];
    cscfp = cmcf->servers.elts;

    for (m = 0; modules[m]; m++) {
        if (modules[m]->type != NGX_LIVE_MODULE) {
            continue;
        }

        module = modules[m]->ctx;
        mi = modules[m]->ctx_index;

        cf->ctx = ctx;

        if (module->init_main_conf) {
            rv = module->init_main_conf(cf, ctx->main_conf[mi]);
            if (rv != NGX_CONF_OK) {
                *cf = pcf;
                return rv;
            }
        }

        for (s = 0; s < cmcf->servers.nelts; s++) {

            /* merge the server{}'s srv_conf's */

            cf->ctx = cscfp[s]->ctx;

            if (module->merge_srv_conf) {
                rv = module->merge_srv_conf(cf,
                                            ctx->srv_conf[mi],
                                            cscfp[s]->ctx->srv_conf[mi]);
                if (rv != NGX_CONF_OK) {
                    *cf = pcf;
                    return rv;
                }
            }

            if (module->merge_app_conf) {

                /* merge the server{}'s app_conf (defaults for its apps) */

                rv = module->merge_app_conf(cf,
                                            ctx->app_conf[mi],
                                            cscfp[s]->ctx->app_conf[mi]);
                if (rv != NGX_CONF_OK) {
                    *cf = pcf;
                    return rv;
                }

                /* merge the applications{}' app_conf's */

                rv = ngx_live_merge_applications(cf, &cscfp[s]->applications,
                                                 cscfp[s]->ctx->app_conf,
                                                 module, mi);
                if (rv != NGX_CONF_OK) {
                    *cf = pcf;
                    return rv;
                }
            }
        }
    }

    cf->ctx = ctx;

    for (m = 0; modules[m]; m++) {
        if (modules[m]->type != NGX_LIVE_MODULE) {
            continue;
        }

        module = modules[m]->ctx;

        if (module->postconfiguration) {
            if (module->postconfiguration(cf) != NGX_OK) {
                *cf = pcf;
                return NGX_CONF_ERROR;
            }
        }
    }

    /* turn rtmp{} listen directives into listening sockets */

    if (ngx_live_open_listening(cf, cmcf) != NGX_OK) {
        *cf = pcf;
        return NGX_CONF_ERROR;
    }

    *cf = pcf;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_live_open_listening(ngx_conf_t *cf, ngx_live_core_main_conf_t *cmcf)
{
    ngx_uint_t                  i;
    ngx_listening_t            *ls;
    ngx_live_listen_t          *listen;
    ngx_live_rtmp_addr_conf_t  *addr_conf;

    listen = cmcf->listening.elts;

    for (i = 0; i < cmcf->listening.nelts; i++) {

        ls = ngx_create_listening(cf,
                                  (struct sockaddr *) listen[i].sockaddr,
                                  listen[i].socklen);
        if (ls == NULL) {
            return NGX_ERROR;
        }

        ls->addr_ntop = 1;
        ls->handler = ngx_live_rtmp_init_connection;
        ls->pool_size = 4096;

#if (NGX_HAVE_INET6)
        ls->ipv6only = listen[i].ipv6only;
#endif

        ls->logp = &cf->cycle->new_log;
        ls->log.data = &ls->addr_text;
        ls->log.handler = ngx_accept_log_error;

        addr_conf = ngx_palloc(cf->pool, sizeof(ngx_live_rtmp_addr_conf_t));
        if (addr_conf == NULL) {
            return NGX_ERROR;
        }

        addr_conf->ctx = listen[i].ctx;

        ls->servers = addr_conf;
    }

    return NGX_OK;
}


static char *
ngx_live_merge_applications(ngx_conf_t *cf, ngx_array_t *applications,
    void **app_conf, ngx_live_module_t *module, ngx_uint_t ctx_index)
{
    char                        *rv;
    ngx_uint_t                   n;
    ngx_live_conf_ctx_t         *ctx, saved;
    ngx_live_core_app_conf_t   **cacfp;

    if (applications == NULL) {
        return NGX_CONF_OK;
    }

    ctx = (ngx_live_conf_ctx_t *) cf->ctx;
    saved = *ctx;

    cacfp = applications->elts;

    for (n = 0; n < applications->nelts; ++n, ++cacfp) {

        ctx->app_conf = (*cacfp)->app_conf;

        rv = module->merge_app_conf(cf, app_conf[ctx_index],
                                    (*cacfp)->app_conf[ctx_index]);
        if (rv != NGX_CONF_OK) {
            return rv;
        }
    }

    *ctx = saved;

    return NGX_CONF_OK;
}
