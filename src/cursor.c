/*
 * cursor.c - bounded, forward-reading cursor over an input buffer.
 *
 * Every read is bounds-checked against [base, base+size). The first failure
 * latches a sticky error that all later reads propagate. Bounds are tested with
 * subtraction (need > size - pos) so no pointer/length arithmetic can overflow.
 */
#include "internal.h"

#include "cursor.h"
#include "varint.h"

/*
 * Ensure n bytes are available at the current position. Returns VL_OK if so;
 * otherwise latches (or propagates) the sticky error and returns it.
 */
static vl_status vl_cursor_need(vl_cursor *c, size_t n) {
    if (VL_UNLIKELY(c->err != VL_OK)) {
        return c->err;
    }
    if (VL_UNLIKELY(n > c->size - c->pos)) {
        c->err = VL_ERR_TRUNCATED;
        return c->err;
    }
    return VL_OK;
}

/* Initialize a cursor over [data, data+size) at position 0 with no error. */
void vl_cursor_init(vl_cursor *c, const uint8_t *data, size_t size) {
    c->base = data;
    c->size = size;
    c->pos = 0;
    c->err = VL_OK;
}

/* Bytes between the current position and the end of the buffer. */
size_t vl_cursor_remaining(const vl_cursor *c) {
    return c->size - c->pos;
}

/* Non-zero once the cursor has consumed the whole buffer. */
int vl_cursor_eof(const vl_cursor *c) {
    return c->pos >= c->size;
}

/* Current sticky status (VL_OK if no read has failed). */
vl_status vl_cursor_status(const vl_cursor *c) {
    return c->err;
}

/* Read one byte little-endian; zero *out and latch on failure. */
vl_status vl_read_u8(vl_cursor *c, uint8_t *out) {
    if (vl_cursor_need(c, 1) != VL_OK) {
        *out = 0;
        return c->err;
    }
    *out = c->base[c->pos];
    c->pos += 1;
    return VL_OK;
}

/* Read a u16 little-endian; zero *out and latch on failure. */
vl_status vl_read_u16(vl_cursor *c, uint16_t *out) {
    if (vl_cursor_need(c, 2) != VL_OK) {
        *out = 0;
        return c->err;
    }
    *out = vl_load_u16le(c->base + c->pos);
    c->pos += 2;
    return VL_OK;
}

/* Read a u32 little-endian; zero *out and latch on failure. */
vl_status vl_read_u32(vl_cursor *c, uint32_t *out) {
    if (vl_cursor_need(c, 4) != VL_OK) {
        *out = 0;
        return c->err;
    }
    *out = vl_load_u32le(c->base + c->pos);
    c->pos += 4;
    return VL_OK;
}

/* Read a u64 little-endian; zero *out and latch on failure. */
vl_status vl_read_u64(vl_cursor *c, uint64_t *out) {
    if (vl_cursor_need(c, 8) != VL_OK) {
        *out = 0;
        return c->err;
    }
    *out = vl_load_u64le(c->base + c->pos);
    c->pos += 8;
    return VL_OK;
}

/* Read an i32 by reinterpreting an unsigned 32-bit read. */
vl_status vl_read_i32(vl_cursor *c, int32_t *out) {
    uint32_t u;
    vl_status s = vl_read_u32(c, &u);
    if (s != VL_OK) {
        *out = 0;
        return s;
    }
    *out = (int32_t)u;
    return VL_OK;
}

/* Read an i64 by reinterpreting an unsigned 64-bit read. */
vl_status vl_read_i64(vl_cursor *c, int64_t *out) {
    uint64_t u;
    vl_status s = vl_read_u64(c, &u);
    if (s != VL_OK) {
        *out = 0;
        return s;
    }
    *out = (int64_t)u;
    return VL_OK;
}

/* Read a 32-bit float: read u32 then bit-copy into float (no aliasing UB). */
vl_status vl_read_f32(vl_cursor *c, float *out) {
    uint32_t u;
    float f;
    vl_status s = vl_read_u32(c, &u);
    if (s != VL_OK) {
        *out = 0;
        return s;
    }
    memcpy(&f, &u, sizeof(f));
    *out = f;
    return VL_OK;
}

/* Read a 64-bit float: read u64 then bit-copy into double (no aliasing UB). */
vl_status vl_read_f64(vl_cursor *c, double *out) {
    uint64_t u;
    double d;
    vl_status s = vl_read_u64(c, &u);
    if (s != VL_OK) {
        *out = 0;
        return s;
    }
    memcpy(&d, &u, sizeof(d));
    *out = d;
    return VL_OK;
}

/*
 * Read an unsigned LEB128 varint over the remaining window. Latches
 * VL_ERR_CORRUPT if the encoding is invalid (or truncated within the buffer).
 */
vl_status vl_read_varint(vl_cursor *c, uint64_t *out) {
    size_t consumed;
    if (VL_UNLIKELY(c->err != VL_OK)) {
        *out = 0;
        return c->err;
    }
    consumed = vl_varint_decode(c->base + c->pos, c->size - c->pos, out);
    if (VL_UNLIKELY(consumed == 0)) {
        *out = 0;
        c->err = VL_ERR_CORRUPT;
        return c->err;
    }
    c->pos += consumed;
    return VL_OK;
}

/* Read a signed varint: unsigned varint followed by zigzag decode. */
vl_status vl_read_svarint(vl_cursor *c, int64_t *out) {
    uint64_t u;
    vl_status s = vl_read_varint(c, &u);
    if (s != VL_OK) {
        *out = 0;
        return s;
    }
    *out = vl_zigzag_decode(u);
    return VL_OK;
}

/* Copy n bytes into dst, advancing the cursor; latch on short read. */
vl_status vl_read_bytes(vl_cursor *c, void *dst, size_t n) {
    if (vl_cursor_need(c, n) != VL_OK) {
        return c->err;
    }
    if (n != 0) {
        memcpy(dst, c->base + c->pos, n);
    }
    c->pos += n;
    return VL_OK;
}

/*
 * Borrow n bytes without copying: store a pointer into the buffer in *out and
 * advance the cursor. On failure zero *out and latch.
 */
vl_status vl_read_slice(vl_cursor *c, size_t n, const uint8_t **out) {
    if (vl_cursor_need(c, n) != VL_OK) {
        *out = NULL;
        return c->err;
    }
    *out = c->base + c->pos;
    c->pos += n;
    return VL_OK;
}

/* Advance the cursor by n bytes without reading them; latch on overrun. */
vl_status vl_skip(vl_cursor *c, size_t n) {
    if (vl_cursor_need(c, n) != VL_OK) {
        return c->err;
    }
    c->pos += n;
    return VL_OK;
}

/* Absolute seek to pos, which must lie in [0, size]; else latch VL_ERR_CORRUPT. */
vl_status vl_seek(vl_cursor *c, size_t pos) {
    if (VL_UNLIKELY(c->err != VL_OK)) {
        return c->err;
    }
    if (VL_UNLIKELY(pos > c->size)) {
        c->err = VL_ERR_CORRUPT;
        return c->err;
    }
    c->pos = pos;
    return VL_OK;
}

/*
 * Create a sub-cursor over the window [off, off+len) of the same buffer. The
 * window bounds are validated with subtraction so no offset arithmetic can
 * overflow. On failure latch on the parent and leave *out zeroed.
 */
vl_status vl_cursor_window(const vl_cursor *c, size_t off, size_t len,
                           vl_cursor *out) {
    vl_cursor_init(out, NULL, 0);
    if (VL_UNLIKELY(c->err != VL_OK)) {
        return c->err;
    }
    if (VL_UNLIKELY(off > c->size || len > c->size - off)) {
        return VL_ERR_CORRUPT;
    }
    vl_cursor_init(out, c->base + off, len);
    return VL_OK;
}
