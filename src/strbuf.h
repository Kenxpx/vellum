/*
 * strbuf.h - a small growable text buffer used to build disassembly and
 * diagnostic strings without repeated reallocation at call sites.
 */
#ifndef VELLUM_STRBUF_H
#define VELLUM_STRBUF_H

#include "internal.h"

typedef struct vl_strbuf {
    char *data;
    size_t len;
    size_t cap;
} vl_strbuf;

void vl_strbuf_init(vl_strbuf *sb);
void vl_strbuf_dispose(vl_strbuf *sb);

/* Append a byte / NUL-terminated string; VL_ERR_OOM on allocation failure. */
vl_status vl_strbuf_putc(vl_strbuf *sb, char c);
vl_status vl_strbuf_puts(vl_strbuf *sb, const char *s);

/* Append formatted text (printf-style). */
vl_status vl_strbuf_printf(vl_strbuf *sb, const char *fmt, ...);

/* NUL-terminated view of the accumulated contents (never NULL). */
const char *vl_strbuf_cstr(vl_strbuf *sb);

#endif /* VELLUM_STRBUF_H */
