/*
 * strbuf.c - a small growable, NUL-terminated text buffer.
 *
 * Implements strbuf.h: a byte buffer that grows geometrically so callers can
 * build disassembly and diagnostic strings without worrying about capacity.
 * The stored capacity always includes room for the trailing NUL terminator,
 * and the contents are kept NUL-terminated after every mutation.
 */
#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vellum/alloc.h"

/*
 * Ensures the buffer can hold at least "need" more content bytes (in addition
 * to the current length) plus the trailing NUL. Grows the capacity by doubling
 * from a minimum of 16, using overflow-checked arithmetic. Returns VL_OK on
 * success or VL_ERR_OOM / VL_ERR_OVERFLOW on failure.
 */
static vl_status vl_strbuf_ensure(vl_strbuf *sb, size_t need)
{
    size_t required;
    size_t newcap;
    char *data;

    /* required = len + need + 1 (the +1 reserves the NUL terminator). */
    if (!vl_size_add(sb->len, need, &required))
        return VL_ERR_OVERFLOW;
    if (!vl_size_add(required, 1, &required))
        return VL_ERR_OVERFLOW;

    if (sb->cap >= required)
        return VL_OK;

    newcap = sb->cap < 16 ? 16 : sb->cap;
    while (newcap < required) {
        if (!vl_size_mul(newcap, 2, &newcap))
            return VL_ERR_OVERFLOW;
    }

    data = vl_realloc(sb->data, newcap);
    if (data == NULL)
        return VL_ERR_OOM;

    sb->data = data;
    sb->cap = newcap;
    return VL_OK;
}

/* Initializes an empty buffer with no backing allocation. */
void vl_strbuf_init(vl_strbuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

/* Frees the backing allocation and resets the buffer to the empty state. */
void vl_strbuf_dispose(vl_strbuf *sb)
{
    vl_free(sb->data);
    vl_strbuf_init(sb);
}

/* Appends a single byte, keeping the buffer NUL-terminated. */
vl_status vl_strbuf_putc(vl_strbuf *sb, char c)
{
    vl_status st = vl_strbuf_ensure(sb, 1);
    if (st != VL_OK)
        return st;

    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
    return VL_OK;
}

/* Appends a NUL-terminated string, keeping the buffer NUL-terminated. */
vl_status vl_strbuf_puts(vl_strbuf *sb, const char *s)
{
    size_t n = strlen(s);
    vl_status st = vl_strbuf_ensure(sb, n);
    if (st != VL_OK)
        return st;

    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return VL_OK;
}

/* Appends printf-style formatted text, keeping the buffer NUL-terminated. */
vl_status vl_strbuf_printf(vl_strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    int n;
    size_t need;
    vl_status st;

    /* First pass: measure the formatted length without writing. */
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0)
        return VL_ERR_IO;

    need = (size_t)n;
    st = vl_strbuf_ensure(sb, need);
    if (st != VL_OK)
        return st;

    /* Second pass: write into the reserved space (room for NUL is included). */
    va_start(ap, fmt);
    vsnprintf(sb->data + sb->len, need + 1, fmt, ap);
    va_end(ap);

    sb->len += need;
    return VL_OK;
}

/* Returns a NUL-terminated view of the contents, or "" when empty. */
const char *vl_strbuf_cstr(vl_strbuf *sb)
{
    return sb->data != NULL ? sb->data : "";
}
