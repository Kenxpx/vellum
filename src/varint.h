/*
 * varint.h - LEB128 variable-length integers and zigzag signed encoding.
 *
 * These operate on raw byte buffers with explicit bounds; the bounded cursor
 * (cursor.h) and the writer (writer.h) build on them.
 */
#ifndef VELLUM_VARINT_H
#define VELLUM_VARINT_H

#include <stddef.h>
#include <stdint.h>

/* Maximum bytes an unsigned 64-bit LEB128 value can occupy. */
#define VL_VARINT_MAX 10

/*
 * Decode an unsigned LEB128 value from buf[0, len). On success stores the
 * value in *out and returns the number of bytes consumed (1..VL_VARINT_MAX).
 * Returns 0 if the buffer ends mid-value or the encoding exceeds 64 bits.
 */
size_t vl_varint_decode(const uint8_t *buf, size_t len, uint64_t *out);

/*
 * Encode v as unsigned LEB128 into buf[0, cap). Returns the number of bytes
 * written, or 0 if cap is too small.
 */
size_t vl_varint_encode(uint64_t v, uint8_t *buf, size_t cap);

/* Number of bytes vl_varint_encode would use for v (1..VL_VARINT_MAX). */
size_t vl_varint_size(uint64_t v);

/* Zigzag map signed<->unsigned so small magnitudes stay small. */
uint64_t vl_zigzag_encode(int64_t v);
int64_t vl_zigzag_decode(uint64_t v);

#endif /* VELLUM_VARINT_H */
