/*
 * varint.c - unsigned LEB128 variable-length integers and zigzag signed
 * encoding. All routines operate on raw byte buffers with explicit bounds;
 * they perform no allocation and never read or write out of range.
 */
#include "internal.h"

#include "varint.h"

/*
 * Decode an unsigned LEB128 value from buf[0, len). Reads 7 value bits per
 * byte, least-significant group first, with the high bit signalling that more
 * bytes follow. On success stores the value in *out and returns the number of
 * bytes consumed (1..VL_VARINT_MAX). Returns 0 if the buffer ends mid-value or
 * the encoding would exceed 64 bits.
 */
size_t vl_varint_decode(const uint8_t *buf, size_t len, uint64_t *out) {
    uint64_t value = 0;
    size_t i;

    for (i = 0; i < len && i < VL_VARINT_MAX; i++) {
        uint8_t byte = buf[i];
        uint64_t group = (uint64_t)(byte & 0x7F);
        unsigned shift = (unsigned)(i * 7);

        /*
         * The final (10th) byte contributes only bit 63; any higher bits set
         * in its 7-bit group would overflow 64 bits.
         */
        if (i == VL_VARINT_MAX - 1 && group > 1) {
            return 0;
        }

        value |= group << shift;

        if ((byte & 0x80) == 0) {
            *out = value;
            return i + 1;
        }
    }

    /* Ran out of buffer, or ten continue-bytes with no terminator: invalid. */
    return 0;
}

/*
 * Encode v as unsigned LEB128 into buf[0, cap). Emits 7 value bits per byte,
 * least-significant group first, setting the high bit on every non-final byte.
 * Returns the number of bytes written, or 0 if cap is too small.
 */
size_t vl_varint_encode(uint64_t v, uint8_t *buf, size_t cap) {
    size_t i = 0;

    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v != 0) {
            byte |= 0x80;
        }
        if (i >= cap) {
            return 0;
        }
        buf[i++] = byte;
    } while (v != 0);

    return i;
}

/*
 * Return the number of bytes vl_varint_encode would use for v (1..10) without
 * writing anything.
 */
size_t vl_varint_size(uint64_t v) {
    size_t n = 1;

    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

/*
 * Zigzag-encode a signed value so that small magnitudes of either sign map to
 * small unsigned values. Uses unsigned shifts on the two's-complement bit
 * pattern to avoid signed-shift undefined behaviour.
 */
uint64_t vl_zigzag_encode(int64_t v) {
    uint64_t u = (uint64_t)v;
    return (u << 1) ^ (uint64_t)(-(int64_t)(u >> 63));
}

/*
 * Inverse of vl_zigzag_encode: recover the signed value from its zigzag
 * unsigned encoding.
 */
int64_t vl_zigzag_decode(uint64_t v) {
    return (int64_t)((v >> 1) ^ (uint64_t)(-(int64_t)(v & 1)));
}
