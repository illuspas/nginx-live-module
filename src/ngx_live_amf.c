
/*
 * Copyright (C) illuspas
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"
#include "ngx_live_amf.h"


static ngx_int_t ngx_live_amf_get_bytes(ngx_live_amf_reader_t *r,
    u_char *dst, size_t n);
static ngx_int_t ngx_live_amf_get_elt(ngx_live_amf_reader_t *r,
    ngx_live_amf_elt_t *elt);
static double ngx_live_amf_swap_double(u_char *p);


static ngx_int_t
ngx_live_amf_has_data(ngx_live_amf_reader_t *r)
{
    while (r->pos == r->last) {
        r->cl = r->cl ? r->cl->next : NULL;

        if (r->cl == NULL) {
            return NGX_DECLINED;
        }

        r->pos = r->cl->buf->pos;
        r->last = r->cl->buf->last;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_live_amf_get_bytes(ngx_live_amf_reader_t *r, u_char *dst, size_t n)
{
    size_t  take;

    while (n) {

        if (r->pos == r->last) {
            r->cl = r->cl ? r->cl->next : NULL;

            if (r->cl == NULL) {
                return NGX_ERROR;
            }

            r->pos = r->cl->buf->pos;
            r->last = r->cl->buf->last;
            continue;
        }

        take = (size_t) (r->last - r->pos);
        take = ngx_min(take, n);

        if (dst) {
            dst = ngx_cpymem(dst, r->pos, take);
        }

        r->pos += take;
        n -= take;
    }

    return NGX_OK;
}


static double
ngx_live_amf_swap_double(u_char *p)
{
    double   v;
    u_char  *q;

    q = (u_char *) &v;

#if (NGX_HAVE_LITTLE_ENDIAN)
    q[0] = p[7];
    q[1] = p[6];
    q[2] = p[5];
    q[3] = p[4];
    q[4] = p[3];
    q[5] = p[2];
    q[6] = p[1];
    q[7] = p[0];
#else
    ngx_memcpy(q, p, 8);
#endif

    return v;
}


/*
 * Read one value.  When elt is NULL, or elt->type differs from the
 * value marker, the value is skipped entirely.
 */

static ngx_int_t
ngx_live_amf_get_elt(ngx_live_amf_reader_t *r, ngx_live_amf_elt_t *elt)
{
    u_char              marker, b[8], nbuf[256];
    uint32_t            i, len;
    ngx_str_t           name;
    ngx_live_amf_elt_t *sub, *se;

    if (ngx_live_amf_get_bytes(r, &marker, 1) != NGX_OK) {
        return NGX_ERROR;
    }

    /* NGX_LIVE_AMF_ANY (0x10000) never matches a marker, so it skips */

    if (elt == NULL || elt->type != marker) {
        elt = NULL;    /* skip this value */
    }

    switch (marker) {

    case NGX_LIVE_AMF_NUMBER:

        if (ngx_live_amf_get_bytes(r, b, 8) != NGX_OK) {
            return NGX_ERROR;
        }

        if (elt) {
            *(double *) elt->data = ngx_live_amf_swap_double(b);
        }

        return NGX_OK;

    case NGX_LIVE_AMF_BOOLEAN:

        if (ngx_live_amf_get_bytes(r, b, 1) != NGX_OK) {
            return NGX_ERROR;
        }

        if (elt) {
            *(ngx_uint_t *) elt->data = b[0] ? 1 : 0;
        }

        return NGX_OK;

    case NGX_LIVE_AMF_STRING:
    case NGX_LIVE_AMF_LONG_STRING:

        len = 0;

        if (marker == NGX_LIVE_AMF_STRING) {
            if (ngx_live_amf_get_bytes(r, b, 2) != NGX_OK) {
                return NGX_ERROR;
            }

            len = ((uint32_t) b[0] << 8) | b[1];

        } else {
            if (ngx_live_amf_get_bytes(r, b, 4) != NGX_OK) {
                return NGX_ERROR;
            }

            len = ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16)
                  | ((uint32_t) b[2] << 8) | b[3];
        }

        if (elt == NULL) {
            return ngx_live_amf_get_bytes(r, NULL, len);
        }

        if (elt->len && len > elt->len) {
            len = elt->len;    /* truncate to the caller's buffer */
        }

        ((ngx_str_t *) elt->data)->len = len;

        if (((ngx_str_t *) elt->data)->data == NULL) {
            ((ngx_str_t *) elt->data)->data = ngx_pnalloc(r->pool, len + 1);
            if (((ngx_str_t *) elt->data)->data == NULL) {
                return NGX_ERROR;
            }
        }

        if (ngx_live_amf_get_bytes(r, ((ngx_str_t *) elt->data)->data, len)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        ((ngx_str_t *) elt->data)->data[len] = '\0';

        return NGX_OK;

    case NGX_LIVE_AMF_NULL:
    case NGX_LIVE_AMF_UNDEFINED:
    case NGX_LIVE_AMF_UNSUPPORTED:
        return NGX_OK;

    case NGX_LIVE_AMF_REFERENCE:
        return ngx_live_amf_get_bytes(r, NULL, 2);

    case NGX_LIVE_AMF_DATE:
        return ngx_live_amf_get_bytes(r, NULL, 10);

    case NGX_LIVE_AMF_OBJECT:
    case NGX_LIVE_AMF_ECMA_ARRAY:

        if (marker == NGX_LIVE_AMF_ECMA_ARRAY) {
            if (ngx_live_amf_get_bytes(r, b, 4) != NGX_OK) {
                return NGX_ERROR;
            }
        }

        sub = elt ? elt->data : NULL;

        for ( ;; ) {

            /* 2-byte name length; 0x0000 + 0x09 ends the object */

            if (ngx_live_amf_get_bytes(r, b, 2) != NGX_OK) {
                return NGX_ERROR;
            }

            if (b[0] == 0 && b[1] == 0) {
                return ngx_live_amf_get_bytes(r, &marker, 1);
            }

            len = ((uint32_t) b[0] << 8) | b[1];

            if (len > sizeof(nbuf)) {
                return NGX_ERROR;
            }

            name.data = nbuf;
            name.len = len;

            if (ngx_live_amf_get_bytes(r, nbuf, len) != NGX_OK) {
                return NGX_ERROR;
            }

            se = NULL;

            if (sub) {
                for (i = 0; i < elt->len; i++) {
                    if (sub[i].name
                        && sub[i].name->len == name.len
                        && ngx_strncmp(sub[i].name->data, name.data, name.len)
                           == 0)
                    {
                        se = &sub[i];
                        break;
                    }
                }
            }

            if (ngx_live_amf_get_elt(r, se) != NGX_OK) {
                return NGX_ERROR;
            }
        }

    case NGX_LIVE_AMF_STRICT_ARRAY:

        if (ngx_live_amf_get_bytes(r, b, 4) != NGX_OK) {
            return NGX_ERROR;
        }

        len = ((uint32_t) b[0] << 24) | ((uint32_t) b[1] << 16)
              | ((uint32_t) b[2] << 8) | b[3];

        if (len > 4096) {
            return NGX_ERROR;
        }

        for (i = 0; i < len; i++) {
            if (ngx_live_amf_get_elt(r, NULL) != NGX_OK) {
                return NGX_ERROR;
            }
        }

        return NGX_OK;

    default:
        ngx_log_debug1(NGX_LOG_DEBUG_ALL, r->log, 0,
                       "live amf: unsupported marker 0x%02Xd", marker);
        return NGX_ERROR;
    }
}


ngx_int_t
ngx_live_amf_read(ngx_live_amf_reader_t *r, ngx_live_amf_elt_t *elts,
    ngx_uint_t nelts)
{
    ngx_uint_t  n;

    for (n = 0; n < nelts; n++) {

        if (ngx_live_amf_has_data(r) != NGX_OK) {
            break;    /* no more values in the message */
        }

        if (ngx_live_amf_get_elt(r, &elts[n]) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


ngx_int_t
ngx_live_amf_read_cmd(ngx_live_amf_reader_t *r, ngx_str_t *name, double *txn)
{
    u_char              marker, b[8];
    uint32_t            len;

    /* command name (string) */

    if (ngx_live_amf_get_bytes(r, &marker, 1) != NGX_OK
        || marker != NGX_LIVE_AMF_STRING
        || ngx_live_amf_get_bytes(r, b, 2) != NGX_OK)
    {
        return NGX_ERROR;
    }

    len = ((uint32_t) b[0] << 8) | b[1];

    name->data = ngx_pnalloc(r->pool, len + 1);
    if (name->data == NULL) {
        return NGX_ERROR;
    }

    name->len = len;

    if (ngx_live_amf_get_bytes(r, name->data, len) != NGX_OK) {
        return NGX_ERROR;
    }

    name->data[len] = '\0';

    /* transaction id (number) */

    if (ngx_live_amf_get_bytes(r, &marker, 1) != NGX_OK
        || marker != NGX_LIVE_AMF_NUMBER
        || ngx_live_amf_get_bytes(r, b, 8) != NGX_OK)
    {
        return NGX_ERROR;
    }

    *txn = ngx_live_amf_swap_double(b);

    return NGX_OK;
}


/*
 * Writer
 */

void
ngx_live_amf_write_number(ngx_live_amf_writer_t *w, double v)
{
    u_char  *p, *q;

    if (ngx_live_amf_writer_full(w)) {
        return;
    }

    if (w->end - w->p < 9) {
        w->p = w->end;
        return;
    }

    *w->p++ = NGX_LIVE_AMF_NUMBER;

    p = (u_char *) &v;
    q = w->p;

#if (NGX_HAVE_LITTLE_ENDIAN)
    q[0] = p[7];
    q[1] = p[6];
    q[2] = p[5];
    q[3] = p[4];
    q[4] = p[3];
    q[5] = p[2];
    q[6] = p[1];
    q[7] = p[0];
#else
    ngx_memcpy(q, p, 8);
#endif

    w->p += 8;
}


void
ngx_live_amf_write_boolean(ngx_live_amf_writer_t *w, ngx_uint_t v)
{
    if (w->end - w->p < 2) {
        w->p = w->end;
        return;
    }

    *w->p++ = NGX_LIVE_AMF_BOOLEAN;
    *w->p++ = v ? 1 : 0;
}


static void
ngx_live_amf_write_str(ngx_live_amf_writer_t *w, u_char *data, size_t len)
{
    if (w->end - w->p < (ssize_t) (len + 2)) {
        w->p = w->end;
        return;
    }

    *w->p++ = (u_char) (len >> 8);
    *w->p++ = (u_char) len;

    w->p = ngx_cpymem(w->p, data, len);
}


void
ngx_live_amf_write_string(ngx_live_amf_writer_t *w, ngx_str_t *s)
{
    if (ngx_live_amf_writer_full(w)) {
        return;
    }

    if (w->end - w->p < 1) {
        w->p = w->end;
        return;
    }

    *w->p++ = NGX_LIVE_AMF_STRING;

    ngx_live_amf_write_str(w, s->data, s->len);
}


void
ngx_live_amf_write_null(ngx_live_amf_writer_t *w)
{
    if (w->end - w->p < 1) {
        w->p = w->end;
        return;
    }

    *w->p++ = NGX_LIVE_AMF_NULL;
}


void
ngx_live_amf_write_object_begin(ngx_live_amf_writer_t *w)
{
    if (w->end - w->p < 1) {
        w->p = w->end;
        return;
    }

    *w->p++ = NGX_LIVE_AMF_OBJECT;
}


void
ngx_live_amf_write_name(ngx_live_amf_writer_t *w, ngx_str_t *name)
{
    ngx_live_amf_write_str(w, name->data, name->len);
}


void
ngx_live_amf_write_object_end(ngx_live_amf_writer_t *w)
{
    if (w->end - w->p < 3) {
        w->p = w->end;
        return;
    }

    *w->p++ = 0;
    *w->p++ = 0;
    *w->p++ = NGX_LIVE_AMF_OBJECT_END;
}
