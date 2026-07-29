/*
 * internal.h - definitions shared across the Vellum VM implementation but not
 * part of the public API. Every .c file in src/ includes this first.
 */
#ifndef VELLUM_INTERNAL_H
#define VELLUM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vellum/alloc.h"
#include "vellum/error.h"

#if defined(__GNUC__) || defined(__clang__)
#  define VL_LIKELY(x) __builtin_expect(!!(x), 1)
#  define VL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define VL_LIKELY(x) (x)
#  define VL_UNLIKELY(x) (x)
#endif

#define VL_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Little-endian scalar loads/stores from a byte buffer; callers guarantee bounds. */
static inline uint16_t vl_load_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t vl_load_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint64_t vl_load_u64le(const uint8_t *p) {
    return (uint64_t)vl_load_u32le(p) | ((uint64_t)vl_load_u32le(p + 4) << 32);
}
static inline void vl_store_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void vl_store_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static inline void vl_store_u64le(uint8_t *p, uint64_t v) {
    vl_store_u32le(p, (uint32_t)(v & 0xFFFFFFFFu));
    vl_store_u32le(p + 4, (uint32_t)(v >> 32));
}

#endif /* VELLUM_INTERNAL_H */
