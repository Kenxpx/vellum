/*
 * cursor.h - a bounded, forward-reading cursor over an input buffer.
 *
 * All untrusted parsing goes through a cursor. Every read is bounds-checked
 * against the buffer; the first out-of-range read latches a sticky error that
 * all subsequent reads propagate, so parsers can read a run of fields and check
 * the error once at the end. Reads never touch memory outside [base, base+size).
 */
#ifndef VELLUM_CURSOR_H
#define VELLUM_CURSOR_H

#include "internal.h"

typedef struct vl_cursor {
    const uint8_t *base;
    size_t size;
    size_t pos;
    vl_status err; /* sticky: first failure is retained */
} vl_cursor;

void vl_cursor_init(vl_cursor *c, const uint8_t *data, size_t size);

/* Bytes between the current position and the end of the buffer. */
size_t vl_cursor_remaining(const vl_cursor *c);

/* Non-zero once the cursor has consumed the whole buffer. */
int vl_cursor_eof(const vl_cursor *c);

/* Current sticky status (VL_OK if no read has failed). */
vl_status vl_cursor_status(const vl_cursor *c);

/*
 * Fixed-width little-endian reads. On success advance the cursor and store the
 * value; on failure latch VL_ERR_TRUNCATED, leave *out zeroed, and return it.
 * A cursor already in error fails fast without touching *out beyond zeroing.
 */
vl_status vl_read_u8(vl_cursor *c, uint8_t *out);
vl_status vl_read_u16(vl_cursor *c, uint16_t *out);
vl_status vl_read_u32(vl_cursor *c, uint32_t *out);
vl_status vl_read_u64(vl_cursor *c, uint64_t *out);

/* Signed/float convenience reads (same width and semantics as above). */
vl_status vl_read_i32(vl_cursor *c, int32_t *out);
vl_status vl_read_i64(vl_cursor *c, int64_t *out);
vl_status vl_read_f32(vl_cursor *c, float *out);
vl_status vl_read_f64(vl_cursor *c, double *out);

/* Unsigned LEB128 varint. Latches VL_ERR_TRUNCATED / VL_ERR_CORRUPT on error. */
vl_status vl_read_varint(vl_cursor *c, uint64_t *out);
vl_status vl_read_svarint(vl_cursor *c, int64_t *out);

/* Copy n bytes into dst, advancing the cursor. */
vl_status vl_read_bytes(vl_cursor *c, void *dst, size_t n);

/*
 * Borrow n bytes without copying: stores a pointer into the buffer in *out and
 * advances the cursor. The pointer is valid for the lifetime of the buffer.
 */
vl_status vl_read_slice(vl_cursor *c, size_t n, const uint8_t **out);

/* Advance the cursor by n bytes without reading them. */
vl_status vl_skip(vl_cursor *c, size_t n);

/* Absolute seek to pos (must be within [0, size]). */
vl_status vl_seek(vl_cursor *c, size_t pos);

/*
 * Create a sub-cursor over the window [off, off+len) of the same buffer, so a
 * section can be parsed without being able to read past its declared bounds.
 */
vl_status vl_cursor_window(const vl_cursor *c, size_t off, size_t len,
                           vl_cursor *out);

#endif /* VELLUM_CURSOR_H */
