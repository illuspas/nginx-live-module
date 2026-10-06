
/*
 * Copyright (C) illuspas
 *
 * Access control for live applications, after ngx_rtmp_access_module:
 *
 *     rtmp {
 *         server {
 *             listen 1935;
 *
 *             application myapp {
 *                 live on;
 *                 allow publish 127.0.0.1;  # only localhost may push
 *                 deny publish all;
 *                 allow play all;           # everyone may watch
 *             }
 *         }
 *     }
 *
 * Rules are evaluated in order, the first matching rule wins, and
 * when no rule matches access is granted.  The rules live in the
 * protocol-neutral rtmp{} block, so HTTP-FLV reuses them unchanged.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"
#include "ngx_live_access_module.h"


typedef struct {
    in_addr_t               mask;
    in_addr_t               addr;
    ngx_uint_t              deny;
    ngx_uint_t              flags;
} ngx_live_access_rule_t;


#if (NGX_HAVE_INET6)

typedef struct {
    struct in6_addr         addr;
    struct in6_addr         mask;
    ngx_uint_t              deny;
    ngx_uint_t              flags;
} ngx_live_access_rule6_t;

#endif


typedef struct {
    ngx_array_t             rules;     /* ngx_live_access_rule_t */
#if (NGX_HAVE_INET6)
    ngx_array_t             rules6;    /* ngx_live_access_rule6_t */
#endif
} ngx_live_access_app_conf_t;


static void *ngx_live_access_create_app_conf(ngx_conf_t *cf);
static char *ngx_live_access_merge_app_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_live_access_rule(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_command_t  ngx_live_access_commands[] = {

    { ngx_string("allow"),
      NGX_LIVE_MAIN_CONF|NGX_LIVE_SRV_CONF|NGX_LIVE_APP_CONF|NGX_CONF_TAKE12,
      ngx_live_access_rule,
      NGX_LIVE_APP_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("deny"),
      NGX_LIVE_MAIN_CONF|NGX_LIVE_SRV_CONF|NGX_LIVE_APP_CONF|NGX_CONF_TAKE12,
      ngx_live_access_rule,
      NGX_LIVE_APP_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_live_module_t  ngx_live_access_module_ctx = {
    NULL,                                   /* preconfiguration */
    NULL,                                   /* postconfiguration */

    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */

    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */

    ngx_live_access_create_app_conf,        /* create app configuration */
    ngx_live_access_merge_app_conf          /* merge app configuration */
};


ngx_module_t  ngx_live_access_module = {
    NGX_MODULE_V1,
    &ngx_live_access_module_ctx,
    ngx_live_access_commands,
    NGX_LIVE_MODULE,
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_live_access_create_app_conf(ngx_conf_t *cf)
{
    ngx_live_access_app_conf_t  *aacf;

    aacf = ngx_pcalloc(cf->pool, sizeof(ngx_live_access_app_conf_t));
    if (aacf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&aacf->rules, cf->pool, 4,
                       sizeof(ngx_live_access_rule_t))
        != NGX_OK)
    {
        return NULL;
    }

#if (NGX_HAVE_INET6)
    if (ngx_array_init(&aacf->rules6, cf->pool, 4,
                       sizeof(ngx_live_access_rule6_t))
        != NGX_OK)
    {
        return NULL;
    }
#endif

    return aacf;
}


static ngx_int_t
ngx_live_access_merge_rules(ngx_array_t *prev, ngx_array_t *rules)
{
    void  *p;

    if (prev->nelts == 0) {
        return NGX_OK;
    }

    if (rules->nelts == 0) {
        *rules = *prev;
        return NGX_OK;
    }

    /* child rules keep precedence, parent rules act as defaults */

    p = ngx_array_push_n(rules, prev->nelts);
    if (p == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(p, prev->elts, prev->size * prev->nelts);

    return NGX_OK;
}


static char *
ngx_live_access_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_live_access_app_conf_t *prev = parent;
    ngx_live_access_app_conf_t *conf = child;

    if (ngx_live_access_merge_rules(&prev->rules, &conf->rules) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

#if (NGX_HAVE_INET6)
    if (ngx_live_access_merge_rules(&prev->rules6, &conf->rules6) != NGX_OK) {
        return NGX_CONF_ERROR;
    }
#endif

    return NGX_CONF_OK;
}


static char *
ngx_live_access_rule(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_int_t                   rc;
    ngx_uint_t                  i, all, deny, flags;
    ngx_str_t                  *value;
    ngx_cidr_t                  cidr;
    ngx_live_access_app_conf_t *aacf = conf;

#if (NGX_HAVE_INET6)
    ngx_live_access_rule6_t    *rule6;
#endif
    ngx_live_access_rule_t     *rule;

    ngx_memzero(&cidr, sizeof(ngx_cidr_t));

    value = cf->args->elts;

    /* deny when the directive name starts with 'd' */
    deny = (value[0].data[0] == 'd');

    flags = 0;

    if (cf->args->nelts == 2) {
        flags = NGX_LIVE_ACCESS_PUBLISH | NGX_LIVE_ACCESS_PLAY;

    } else {

        for (i = 1; i < cf->args->nelts - 1; i++) {

            if (value[i].len == sizeof("publish") - 1
                && ngx_strcmp(value[i].data, "publish") == 0)
            {
                flags |= NGX_LIVE_ACCESS_PUBLISH;
                continue;
            }

            if (value[i].len == sizeof("play") - 1
                && ngx_strcmp(value[i].data, "play") == 0)
            {
                flags |= NGX_LIVE_ACCESS_PLAY;
                continue;
            }

            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid parameter \"%V\", expected "
                               "\"publish\" or \"play\"", &value[i]);
            return NGX_CONF_ERROR;
        }
    }

    value += cf->args->nelts - 1;    /* the address argument */

    all = (value->len == 3 && ngx_strcmp(value->data, "all") == 0);

    if (!all) {

        rc = ngx_ptocidr(value, &cidr);

        if (rc == NGX_ERROR) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid parameter \"%V\"", value);
            return NGX_CONF_ERROR;
        }

        if (rc == NGX_DONE) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "low address bits of %V are meaningless",
                               value);
        }
    }

    if (cidr.family == AF_INET || all) {

        rule = ngx_array_push(&aacf->rules);
        if (rule == NULL) {
            return NGX_CONF_ERROR;
        }

        rule->mask = cidr.u.in.mask;
        rule->addr = cidr.u.in.addr;
        rule->deny = deny;
        rule->flags = flags;
    }

#if (NGX_HAVE_INET6)
    if (cidr.family == AF_INET6 || all) {

        rule6 = ngx_array_push(&aacf->rules6);
        if (rule6 == NULL) {
            return NGX_CONF_ERROR;
        }

        rule6->mask = cidr.u.in6.mask;
        rule6->addr = cidr.u.in6.addr;
        rule6->deny = deny;
        rule6->flags = flags;
    }
#endif

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_live_access_found(ngx_log_t *log, ngx_uint_t deny)
{
    if (deny) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "live: access forbidden by rule");
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_live_access_inet(ngx_live_access_app_conf_t *aacf, ngx_log_t *log,
    in_addr_t addr, ngx_uint_t flag)
{
    ngx_uint_t              i;
    ngx_live_access_rule_t *rule;

    rule = aacf->rules.elts;

    for (i = 0; i < aacf->rules.nelts; i++) {

        if ((addr & rule[i].mask) == rule[i].addr
            && (flag & rule[i].flags))
        {
            return ngx_live_access_found(log, rule[i].deny);
        }
    }

    return NGX_OK;
}


#if (NGX_HAVE_INET6)

static ngx_int_t
ngx_live_access_inet6(ngx_live_access_app_conf_t *aacf, ngx_log_t *log,
    u_char *p, ngx_uint_t flag)
{
    ngx_uint_t               i, n;
    ngx_live_access_rule6_t *rule6;

    rule6 = aacf->rules6.elts;

    for (i = 0; i < aacf->rules6.nelts; i++) {

        for (n = 0; n < 16; n++) {
            if ((p[n] & rule6[i].mask.s6_addr[n])
                != rule6[i].addr.s6_addr[n])
            {
                goto next;
            }
        }

        if (flag & rule6[i].flags) {
            return ngx_live_access_found(log, rule6[i].deny);
        }

    next:
        continue;
    }

    return NGX_OK;
}

#endif


ngx_int_t
ngx_live_access_permit(ngx_live_core_app_conf_t *cacf, ngx_connection_t *c,
    ngx_uint_t flag)
{
    ngx_live_access_app_conf_t   *aacf;

#if (NGX_HAVE_INET6)
    u_char                       *p;
    in_addr_t                     addr;
    struct sockaddr_in6          *sin6;
#endif
    struct sockaddr_in           *sin;

    if (cacf == NULL || c == NULL) {
        return NGX_OK;
    }

    aacf = cacf->app_conf[ngx_live_access_module.ctx_index];

    if (aacf == NULL || c->sockaddr == NULL) {
        return NGX_OK;
    }

    switch (c->sockaddr->sa_family) {

    case AF_INET:
        sin = (struct sockaddr_in *) c->sockaddr;
        return ngx_live_access_inet(aacf, c->log, sin->sin_addr.s_addr, flag);

#if (NGX_HAVE_INET6)

    case AF_INET6:
        sin6 = (struct sockaddr_in6 *) c->sockaddr;
        p = sin6->sin6_addr.s6_addr;

        if (IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr)) {
            addr = p[12] << 24;
            addr += p[13] << 16;
            addr += p[14] << 8;
            addr += p[15];

            return ngx_live_access_inet(aacf, c->log, htonl(addr), flag);
        }

        return ngx_live_access_inet6(aacf, c->log, p, flag);

#endif
    }

    return NGX_OK;
}
