/*
 * test_hashmap.c - unit tests for the open-addressing hashmap (hashmap.h) and
 * the FNV-1a byte hash. Exercises many inserts (forcing several grows), hits,
 * misses, overwrite, and vl_hash_bytes determinism.
 */
#include "hashmap.h"
#include "harness.h"

#define N 1000

/* Insert N keys, force grows, verify gets, misses, and overwrite. */
int main(void)
{
    vl_hashmap m;
    vl_hashmap_init(&m);

    /* Insert N keys of the form i*2654435761u with value i. */
    for (uint32_t i = 0; i < N; i++) {
        uint64_t key = (uint64_t)i * 2654435761u;
        CHECK_OK(vl_hashmap_put(&m, key, i));
    }
    CHECK_EQ(m.count, N);
    CHECK(m.cap >= N);

    /* Every inserted key must resolve to its value. */
    for (uint32_t i = 0; i < N; i++) {
        uint64_t key = (uint64_t)i * 2654435761u;
        uint32_t out = 0xDEADBEEFu;
        CHECK_EQ(vl_hashmap_get(&m, key, &out), 1);
        CHECK_EQ(out, i);
    }

    /* Absent keys must miss and leave *out untouched. */
    for (uint32_t i = 0; i < N; i++) {
        uint64_t key = (uint64_t)i * 2654435761u + 1u;
        uint32_t out = 0x12345678u;
        /* Guard against accidental collision with a real key. */
        int is_real = 0;
        for (uint32_t j = 0; j < N; j++) {
            if ((uint64_t)j * 2654435761u == key) {
                is_real = 1;
                break;
            }
        }
        if (is_real) {
            continue;
        }
        CHECK_EQ(vl_hashmap_get(&m, key, &out), 0);
        CHECK_EQ(out, 0x12345678u);
    }

    /* Overwrite updates the value without changing the count. */
    {
        uint64_t key = (uint64_t)500u * 2654435761u;
        uint32_t out = 0;
        CHECK_OK(vl_hashmap_put(&m, key, 999u));
        CHECK_EQ(m.count, N);
        CHECK_EQ(vl_hashmap_get(&m, key, &out), 1);
        CHECK_EQ(out, 999u);
    }

    vl_hashmap_dispose(&m);

    /* vl_hash_bytes is deterministic for the same input. */
    {
        const char *a = "hello world";
        const char *b = "hello worlD";
        uint64_t h1 = vl_hash_bytes(a, 11);
        uint64_t h2 = vl_hash_bytes(a, 11);
        CHECK(h1 == h2);

        /* Two different short buffers should hash differently. */
        uint64_t hb = vl_hash_bytes(b, 11);
        CHECK(h1 != hb);

        /* Empty range is deterministic. */
        CHECK(vl_hash_bytes(a, 0) == vl_hash_bytes(b, 0));
    }

    return TEST_SUMMARY();
}
