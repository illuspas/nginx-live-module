
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_ACCESS_MODULE_H_INCLUDED_
#define _NGX_LIVE_ACCESS_MODULE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"
#include "ngx_live_core_module.h"


#define NGX_LIVE_ACCESS_PUBLISH     0x01
#define NGX_LIVE_ACCESS_PLAY        0x02


/*
 * Check the client address against the rules configured in the
 * application block.  The first matching rule wins; when no rule
 * matches the client is allowed (same semantics as
 * ngx_rtmp_access_module).
 *
 * Returns NGX_OK when permitted, NGX_ERROR when denied.
 */
ngx_int_t ngx_live_access_permit(ngx_live_core_app_conf_t *cacf,
    ngx_connection_t *c, ngx_uint_t flag);


extern ngx_module_t ngx_live_access_module;


#endif /* _NGX_LIVE_ACCESS_MODULE_H_INCLUDED_ */
