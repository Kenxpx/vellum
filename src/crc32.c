/*
 * crc32.c - CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
 *
 * Standard reflected CRC-32: init 0xFFFFFFFF, process each byte low-bit first,
 * final XOR 0xFFFFFFFF. vl_crc32("123456789", 9) == 0xCBF43926.
 */
#include "internal.h"

#include "crc32.h"

/* Lazily-initialized 256-entry lookup table for the reflected polynomial. */
static uint32_t vl_crc32_table[256];
static int vl_crc32_table_ready;

/* Populate vl_crc32_table for polynomial 0xEDB88320 (idempotent). */
static void vl_crc32_build_table(void) {
    uint32_t i;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        int k;
        for (k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        vl_crc32_table[i] = c;
    }
    vl_crc32_table_ready = 1;
}

/*
 * Incremental CRC-32. The incoming crc is a previously returned (finalized)
 * CRC value; it is inverted back to running state, updated with the new bytes,
 * then re-finalized. Seed the first call with 0.
 */
uint32_t vl_crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t running;
    size_t i;

    if (VL_UNLIKELY(!vl_crc32_table_ready)) {
        vl_crc32_build_table();
    }

    running = ~crc;
    for (i = 0; i < len; i++) {
        running = vl_crc32_table[(running ^ p[i]) & 0xFFu] ^ (running >> 8);
    }
    return ~running;
}

/* One-shot CRC-32 over a buffer. */
uint32_t vl_crc32(const void *data, size_t len) {
    return vl_crc32_update(0, data, len);
}
