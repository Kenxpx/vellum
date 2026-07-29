/*
 * hashmap.c - open-addressing (linear-probe) map from 64-bit keys to 32-bit
 * values, plus the 64-bit FNV-1a hash used to derive those keys. Capacity is
 * always a power of two so the probe index masks cheaply; the table grows and
 * rehashes before it gets denser than 70%.
 */
#include "internal.h"

#include <assert.h>

#include "hashmap.h"

/* Compute the 64-bit FNV-1a hash of a byte range. */
uint64_t vl_hash_bytes(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037u;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211u;
    }
    return h;
}

/* Initialize an empty map. */
void vl_hashmap_init(vl_hashmap *m)
{
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
}

/* Free the map's storage and reset it to the empty state. */
void vl_hashmap_dispose(vl_hashmap *m)
{
    vl_free(m->slots);
    vl_hashmap_init(m);
}

/*
 * Insert key->val into a table known to have a free slot; never grows. Used
 * internally by the public put and by rehash. Overwrites an existing entry.
 */
static void vl_hashmap_insert(vl_hm_slot *slots, size_t cap, size_t *count,
                              uint64_t key, uint32_t val)
{
    size_t mask = cap - 1;
    size_t i = (size_t)key & mask;

    for (;;) {
        if (!slots[i].used) {
            slots[i].key = key;
            slots[i].val = val;
            slots[i].used = 1;
            (*count)++;
            return;
        }
        if (slots[i].key == key) {
            slots[i].val = val;
            return;
        }
        i = (i + 1) & mask;
    }
}

/* Grow the table to new_cap (a power of two) and rehash every entry. */
static vl_status vl_hashmap_grow(vl_hashmap *m, size_t new_cap)
{
    vl_hm_slot *slots;
    size_t new_count = 0;
    size_t i;

    slots = (vl_hm_slot *)vl_calloc(new_cap, sizeof(vl_hm_slot));
    if (!slots) {
        return VL_ERR_OOM;
    }

    for (i = 0; i < m->cap; i++) {
        if (m->slots[i].used) {
            vl_hashmap_insert(slots, new_cap, &new_count, m->slots[i].key,
                              m->slots[i].val);
        }
    }

    vl_free(m->slots);
    m->slots = slots;
    m->cap = new_cap;
    m->count = new_count;
    return VL_OK;
}

/* Insert or overwrite key->val, growing the table when it gets too dense. */
vl_status vl_hashmap_put(vl_hashmap *m, uint64_t key, uint32_t val)
{
    assert(val != VL_HASHMAP_EMPTY);

    /* Grow when load factor would reach 70%, and when the table is empty. */
    if (m->cap == 0 || (m->count + 1) * 10 >= m->cap * 7) {
        size_t new_cap = (m->cap == 0) ? 16 : m->cap * 2;
        vl_status st = vl_hashmap_grow(m, new_cap);
        if (st != VL_OK) {
            return st;
        }
    }

    vl_hashmap_insert(m->slots, m->cap, &m->count, key, val);
    return VL_OK;
}

/* Look up key; on hit set *out and return 1, else return 0. */
int vl_hashmap_get(const vl_hashmap *m, uint64_t key, uint32_t *out)
{
    size_t mask;
    size_t i;

    if (m->cap == 0) {
        return 0;
    }

    mask = m->cap - 1;
    i = (size_t)key & mask;
    for (;;) {
        if (!m->slots[i].used) {
            return 0;
        }
        if (m->slots[i].key == key) {
            *out = m->slots[i].val;
            return 1;
        }
        i = (i + 1) & mask;
    }
}
