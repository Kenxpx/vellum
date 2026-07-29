/*
 * hashmap.h - a small open-addressing hash map from 64-bit keys to 32-bit
 * values, used for string interning and type-id lookup. Linear probing with
 * power-of-two capacity and a tombstone-free (insert/lookup only) interface,
 * which is all the loader needs.
 */
#ifndef VELLUM_HASHMAP_H
#define VELLUM_HASHMAP_H

#include "internal.h"

#define VL_HASHMAP_EMPTY 0xFFFFFFFFu /* reserved "no value" sentinel */

typedef struct vl_hm_slot {
    uint64_t key;
    uint32_t val;
    uint32_t used; /* 0 = empty, 1 = occupied */
} vl_hm_slot;

typedef struct vl_hashmap {
    vl_hm_slot *slots;
    size_t cap;   /* power of two, or 0 when empty */
    size_t count; /* occupied slots */
} vl_hashmap;

void vl_hashmap_init(vl_hashmap *m);
void vl_hashmap_dispose(vl_hashmap *m);

/*
 * Insert or overwrite key->val. Returns VL_OK, or VL_ERR_OOM if a needed grow
 * fails. val must not equal VL_HASHMAP_EMPTY.
 */
vl_status vl_hashmap_put(vl_hashmap *m, uint64_t key, uint32_t val);

/*
 * Look up key. On hit stores the value in *out and returns 1; on miss returns 0
 * and leaves *out untouched.
 */
int vl_hashmap_get(const vl_hashmap *m, uint64_t key, uint32_t *out);

/* A stable 64-bit hash of a byte range (FNV-1a). */
uint64_t vl_hash_bytes(const void *data, size_t len);

#endif /* VELLUM_HASHMAP_H */
