/*
 * test_varint.c - unit tests for varint.h (LEB128 + zigzag).
 */
#include "harness.h"
#include "varint.h"

#include <stdint.h>

/* Encode then decode v, asserting the round-trip and size consistency. */
static void check_roundtrip(uint64_t v)
{
    uint8_t buf[VL_VARINT_MAX];
    size_t n = vl_varint_encode(v, buf, sizeof(buf));
    CHECK(n >= 1 && n <= VL_VARINT_MAX);
    CHECK_EQ(vl_varint_size(v), n);

    uint64_t out = 0;
    size_t m = vl_varint_decode(buf, n, &out);
    CHECK_EQ(m, n);
    CHECK(out == v);

    /* A buffer truncated by one byte must fail to decode. */
    if (n > 0) {
        uint64_t tout = 0;
        CHECK_EQ(vl_varint_decode(buf, n - 1, &tout), 0);
    }
}

/* Encode s via zigzag, decode back, asserting the signed round-trip. */
static void check_zigzag(int64_t s)
{
    uint64_t z = vl_zigzag_encode(s);
    CHECK(vl_zigzag_decode(z) == s);
}

/* Exercise round-trips, size agreement, truncation, and zigzag. */
int main(void)
{
    check_roundtrip(0);
    check_roundtrip(1);
    check_roundtrip(127);
    check_roundtrip(128);
    check_roundtrip(300);
    check_roundtrip(0xFFFFFFFFu);
    check_roundtrip(0xFFFFFFFFFFFFFFFFull);

    /* Encoding into a too-small buffer returns 0. */
    uint8_t small[1];
    CHECK_EQ(vl_varint_encode(128, small, 0), 0);
    CHECK_EQ(vl_varint_encode(128, small, sizeof(small)), 0);

    /* Decoding an empty buffer returns 0. */
    uint64_t out = 0;
    CHECK_EQ(vl_varint_decode(NULL, 0, &out), 0);

    check_zigzag(0);
    check_zigzag(-1);
    check_zigzag(1);
    check_zigzag(-2);
    check_zigzag(2);
    check_zigzag(INT64_MIN);
    check_zigzag(INT64_MAX);

    /* Known zigzag mappings. */
    CHECK_EQ(vl_zigzag_encode(0), 0);
    CHECK_EQ(vl_zigzag_encode(-1), 1);
    CHECK_EQ(vl_zigzag_encode(1), 2);
    CHECK_EQ(vl_zigzag_encode(-2), 3);

    return TEST_SUMMARY();
}
