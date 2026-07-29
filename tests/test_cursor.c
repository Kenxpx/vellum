/*
 * test_cursor.c - unit tests for the bounded reading cursor (cursor.h).
 */
#include "vellum/error.h"
#include "cursor.h"
#include "varint.h"
#include "harness.h"

/* Read fixed-width LE values back and verify values and cursor positions. */
static void test_fixed_reads(void)
{
    uint8_t buf[15];
    vl_cursor c;
    uint8_t v8;
    uint16_t v16;
    uint32_t v32;
    uint64_t v64;

    /* u8 = 0xAB */
    buf[0] = 0xAB;
    /* u16 = 0xBEEF (LE: EF BE) */
    buf[1] = 0xEF;
    buf[2] = 0xBE;
    /* u32 = 0xDEADBEEF (LE: EF BE AD DE) */
    buf[3] = 0xEF;
    buf[4] = 0xBE;
    buf[5] = 0xAD;
    buf[6] = 0xDE;
    /* u64 = 0x0123456789ABCDEF (LE) */
    buf[7] = 0xEF;
    buf[8] = 0xCD;
    buf[9] = 0xAB;
    buf[10] = 0x89;
    buf[11] = 0x67;
    buf[12] = 0x45;
    buf[13] = 0x23;
    buf[14] = 0x01;

    vl_cursor_init(&c, buf, sizeof(buf));
    CHECK_EQ(vl_cursor_remaining(&c), (long long)sizeof(buf));
    CHECK_EQ(vl_cursor_eof(&c), 0);

    CHECK_OK(vl_read_u8(&c, &v8));
    CHECK_EQ(v8, 0xAB);
    CHECK_EQ(c.pos, 1);

    CHECK_OK(vl_read_u16(&c, &v16));
    CHECK_EQ(v16, 0xBEEF);
    CHECK_EQ(c.pos, 3);

    CHECK_OK(vl_read_u32(&c, &v32));
    CHECK_EQ(v32, 0xDEADBEEFu);
    CHECK_EQ(c.pos, 7);

    CHECK_OK(vl_read_u64(&c, &v64));
    CHECK_EQ((long long)v64, (long long)0x0123456789ABCDEFULL);
    CHECK_EQ(c.pos, 15);

    CHECK_EQ(vl_cursor_remaining(&c), 0);
    CHECK(vl_cursor_eof(&c));
    CHECK_OK(vl_cursor_status(&c));
}

/* Varint round-trips through the cursor. */
static void test_varint_reads(void)
{
    uint8_t buf[32];
    vl_cursor c;
    size_t n;
    uint64_t v;
    int64_t sv;
    size_t off = 0;

    /* 0 -> single byte */
    n = vl_varint_encode(0, buf + off, sizeof(buf) - off);
    CHECK(n > 0);
    off += n;
    /* 300 -> two bytes */
    n = vl_varint_encode(300, buf + off, sizeof(buf) - off);
    CHECK(n > 0);
    off += n;
    /* large value */
    n = vl_varint_encode(0xFFFFFFFFFFULL, buf + off, sizeof(buf) - off);
    CHECK(n > 0);
    off += n;
    /* signed via zigzag */
    n = vl_varint_encode(vl_zigzag_encode(-12345), buf + off, sizeof(buf) - off);
    CHECK(n > 0);
    off += n;

    vl_cursor_init(&c, buf, off);

    CHECK_OK(vl_read_varint(&c, &v));
    CHECK_EQ((long long)v, 0);
    CHECK_OK(vl_read_varint(&c, &v));
    CHECK_EQ((long long)v, 300);
    CHECK_OK(vl_read_varint(&c, &v));
    CHECK_EQ((long long)v, (long long)0xFFFFFFFFFFULL);
    CHECK_OK(vl_read_svarint(&c, &sv));
    CHECK_EQ((long long)sv, -12345);

    CHECK(vl_cursor_eof(&c));
}

/* Reading past the end latches VL_ERR_TRUNCATED and zeroes outputs. */
static void test_truncation(void)
{
    uint8_t buf[3] = { 0x11, 0x22, 0x33 };
    vl_cursor c;
    uint32_t v32 = 0x55555555u;
    uint8_t v8 = 0x99;

    vl_cursor_init(&c, buf, sizeof(buf));

    /* Only 3 bytes remain: a u32 read must fail and zero *out. */
    CHECK_EQ(vl_read_u32(&c, &v32), VL_ERR_TRUNCATED);
    CHECK_EQ(v32, 0);
    /* Sticky: cursor stays in error and position is unchanged by failure. */
    CHECK_EQ(vl_cursor_status(&c), VL_ERR_TRUNCATED);

    /* A subsequent read fails fast and still zeroes its output. */
    CHECK_EQ(vl_read_u8(&c, &v8), VL_ERR_TRUNCATED);
    CHECK_EQ(v8, 0);
}

/* An exact-fit read succeeds, then one more byte truncates. */
static void test_exact_then_over(void)
{
    uint8_t buf[2] = { 0xAA, 0xBB };
    vl_cursor c;
    uint16_t v16 = 0;
    uint8_t v8 = 0x7;

    vl_cursor_init(&c, buf, sizeof(buf));
    CHECK_OK(vl_read_u16(&c, &v16));
    CHECK_EQ(v16, 0xBBAA);
    CHECK(vl_cursor_eof(&c));

    CHECK_EQ(vl_read_u8(&c, &v8), VL_ERR_TRUNCATED);
    CHECK_EQ(v8, 0);
}

/* vl_read_slice returns an interior pointer into the buffer. */
static void test_slice(void)
{
    uint8_t buf[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    vl_cursor c;
    const uint8_t *p = NULL;

    vl_cursor_init(&c, buf, sizeof(buf));
    CHECK_OK(vl_skip(&c, 2));
    CHECK_EQ(c.pos, 2);

    CHECK_OK(vl_read_slice(&c, 3, &p));
    CHECK(p == buf + 2);
    CHECK_EQ(p[0], 2);
    CHECK_EQ(p[2], 4);
    CHECK_EQ(c.pos, 5);

    /* A slice past the end fails and does not advance. */
    p = NULL;
    CHECK_EQ(vl_read_slice(&c, 100, &p), VL_ERR_TRUNCATED);
}

/* vl_read_bytes copies out, and rejects an over-long request. */
static void test_read_bytes(void)
{
    uint8_t buf[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    vl_cursor c;
    uint8_t dst[4] = { 0, 0, 0, 0 };

    vl_cursor_init(&c, buf, sizeof(buf));
    CHECK_OK(vl_read_bytes(&c, dst, 4));
    CHECK_EQ(dst[0], 0xDE);
    CHECK_EQ(dst[3], 0xEF);
    CHECK(vl_cursor_eof(&c));

    vl_cursor_init(&c, buf, sizeof(buf));
    CHECK_EQ(vl_read_bytes(&c, dst, 5), VL_ERR_TRUNCATED);
}

/* vl_seek accepts positions within [0,size] and rejects beyond. */
static void test_seek(void)
{
    uint8_t buf[4] = { 1, 2, 3, 4 };
    vl_cursor c;
    uint8_t v8;

    vl_cursor_init(&c, buf, sizeof(buf));
    CHECK_OK(vl_seek(&c, 4)); /* end is a valid position */
    CHECK(vl_cursor_eof(&c));
    CHECK_OK(vl_seek(&c, 1));
    CHECK_OK(vl_read_u8(&c, &v8));
    CHECK_EQ(v8, 2);

    CHECK(vl_seek(&c, 5) != VL_OK);
}

/* vl_cursor_window rejects out-of-range windows and accepts valid ones. */
static void test_window(void)
{
    uint8_t buf[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    vl_cursor c;
    vl_cursor w;
    uint8_t v8;

    vl_cursor_init(&c, buf, sizeof(buf));

    /* Valid interior window [3, 3+4). */
    CHECK_OK(vl_cursor_window(&c, 3, 4, &w));
    CHECK_EQ(vl_cursor_remaining(&w), 4);
    CHECK_OK(vl_read_u8(&w, &v8));
    CHECK_EQ(v8, 3);
    /* Window cannot read beyond its declared length. */
    CHECK_OK(vl_skip(&w, 3));
    CHECK(vl_cursor_eof(&w));
    CHECK_EQ(vl_read_u8(&w, &v8), VL_ERR_TRUNCATED);

    /* Whole-buffer window is valid. */
    CHECK_OK(vl_cursor_window(&c, 0, 10, &w));
    CHECK_EQ(vl_cursor_remaining(&w), 10);

    /* Zero-length window at the end is valid. */
    CHECK_OK(vl_cursor_window(&c, 10, 0, &w));
    CHECK_EQ(vl_cursor_remaining(&w), 0);

    /* Out-of-range: offset past end. */
    CHECK(vl_cursor_window(&c, 11, 0, &w) != VL_OK);
    /* Out-of-range: length runs past end. */
    CHECK(vl_cursor_window(&c, 8, 4, &w) != VL_OK);
    /* Out-of-range: offset+len overflow-ish / beyond size. */
    CHECK(vl_cursor_window(&c, 0, 11, &w) != VL_OK);
}

int main(void)
{
    test_fixed_reads();
    test_varint_reads();
    test_truncation();
    test_exact_then_over();
    test_slice();
    test_read_bytes();
    test_seek();
    test_window();
    return TEST_SUMMARY();
}
