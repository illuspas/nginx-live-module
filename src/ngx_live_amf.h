
/*
 * Copyright (C) illuspas
 */


#ifndef _NGX_LIVE_AMF_H_INCLUDED_
#define _NGX_LIVE_AMF_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include "ngx_live.h"


/* AMF0 type markers */

#define NGX_LIVE_AMF_NUMBER          0x00
#define NGX_LIVE_AMF_BOOLEAN         0x01
#define NGX_LIVE_AMF_STRING          0x02
#define NGX_LIVE_AMF_OBJECT          0x03
#define NGX_LIVE_AMF_NULL            0x05
#define NGX_LIVE_AMF_UNDEFINED       0x06
#define NGX_LIVE_AMF_REFERENCE       0x07
#define NGX_LIVE_AMF_ECMA_ARRAY      0x08
#define NGX_LIVE_AMF_OBJECT_END      0x09
#define NGX_LIVE_AMF_STRICT_ARRAY    0x0a
#define NGX_LIVE_AMF_DATE            0x0b
#define NGX_LIVE_AMF_LONG_STRING     0x0c
#define NGX_LIVE_AMF_UNSUPPORTED     0x0d

/* pseudo type: matches (and skips) a value of any type */
#define NGX_LIVE_AMF_ANY             0x10000


/*
 * Element descriptor.  An array of elements describes the expected
 * sequence of AMF0 values (or object members) to decode.
 *
 *   type       expected AMF0 marker
 *   name       object member name (NULL for positional elements)
 *   data       destination (see below), or NULL to skip the value
 *   len        for STRING: capacity of the target buffer;
 *              for OBJECT/ECMA_ARRAY: number of nested elements
 *   mandatory  decoding fails when a mandatory element does not match
 *
 * Destinations:
 *   NUMBER   -> double *
 *   BOOLEAN  -> ngx_uint_t *  (0 or 1)
 *   STRING   -> ngx_str_t *   (data allocated from the reader pool
 *                              when its own data is NULL, copied
 *                              otherwise; truncated to len)
 *   OBJECT /
 *   ECMA_ARRAY -> ngx_live_amf_elt_t * nested element array
 *
 * An element with type == 0 skips one value of any type.
 */

typedef struct {
    ngx_uint_t              type;
    ngx_str_t              *name;
    void                   *data;
    size_t                  len;
    ngx_uint_t              mandatory:1;
} ngx_live_amf_elt_t;


typedef struct {
    ngx_pool_t             *pool;      /* string allocations */
    ngx_log_t              *log;
    ngx_chain_t            *cl;        /* message body cursor */
    u_char                 *pos;
    u_char                 *last;
} ngx_live_amf_reader_t;


/*
 * Decode the expected elements from the reader position.  Values
 * whose type does not match the element are skipped; a mandatory
 * mismatch is an error.  Returns NGX_OK or NGX_ERROR.
 */
ngx_int_t ngx_live_amf_read(ngx_live_amf_reader_t *r,
    ngx_live_amf_elt_t *elts, ngx_uint_t nelts);

/* read the leading command name + transaction id of an invoke */
ngx_int_t ngx_live_amf_read_cmd(ngx_live_amf_reader_t *r, ngx_str_t *name,
    double *txn);


/*
 * Encoder over a caller-provided byte area.  Every put function is
 * bounds-checked; on overflow the cursor sticks at the end.  Check
 * ngx_live_amf_writer_full() after encoding.
 */

typedef struct {
    u_char                 *p;
    u_char                 *end;
} ngx_live_amf_writer_t;


#define ngx_live_amf_writer_full(w)  ((w)->p >= (w)->end)


void ngx_live_amf_write_number(ngx_live_amf_writer_t *w, double v);
void ngx_live_amf_write_boolean(ngx_live_amf_writer_t *w, ngx_uint_t v);
void ngx_live_amf_write_string(ngx_live_amf_writer_t *w, ngx_str_t *s);
void ngx_live_amf_write_null(ngx_live_amf_writer_t *w);
void ngx_live_amf_write_object_begin(ngx_live_amf_writer_t *w);
void ngx_live_amf_write_name(ngx_live_amf_writer_t *w, ngx_str_t *name);
void ngx_live_amf_write_object_end(ngx_live_amf_writer_t *w);


#endif /* _NGX_LIVE_AMF_H_INCLUDED_ */
