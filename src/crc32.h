/*
 * crc32.h - CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
 *
 * Used for the advisory header checksum and optional per-section checksums.
 * The checksums are informational: a mismatch is reported by the validator but
 * does not by itself stop a load, so the format is not gated on a single
 * checksum value.
 */
#ifndef VELLUM_CRC32_H
#define VELLUM_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* One-shot CRC-32 over a buffer. */
uint32_t vl_crc32(const void *data, size_t len);

/* Incremental CRC-32: seed the first call with 0. */
uint32_t vl_crc32_update(uint32_t crc, const void *data, size_t len);

#endif /* VELLUM_CRC32_H */
