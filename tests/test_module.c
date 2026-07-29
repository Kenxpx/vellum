/*
 * test_module.c - unit tests for the .qbc module loader (module.h).
 *
 * Builds a small but valid module image in memory by hand, following the
 * byte-level layout in docs/BYTECODE.md, loads it, and checks that every
 * field round-trips. Also checks that a truncated image and a bad magic are
 * rejected with the right status rather than crashing.
 */
#include "harness.h"

#include "internal.h"
#include "module.h"
#include "opcodes.h"

/* A tiny growable-free byte buffer: fixed capacity is plenty for these tests. */
typedef struct buf {
    uint8_t data[512];
    size_t len;
} buf;

/* Append one byte. */
static void put_u8(buf *b, uint8_t v) {
    b->data[b->len++] = v;
}

/* Append a little-endian u16. */
static void put_u16(buf *b, uint16_t v) {
    vl_store_u16le(b->data + b->len, v);
    b->len += 2;
}

/* Append a little-endian u32. */
static void put_u32(buf *b, uint32_t v) {
    vl_store_u32le(b->data + b->len, v);
    b->len += 4;
}

/* Append a little-endian u64. */
static void put_u64(buf *b, uint64_t v) {
    vl_store_u64le(b->data + b->len, v);
    b->len += 8;
}

/* Append an unsigned LEB128 varint. */
static void put_varint(buf *b, uint64_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v != 0) {
            byte |= 0x80;
        }
        put_u8(b, byte);
    } while (v != 0);
}

/* Append raw bytes. */
static void put_bytes(buf *b, const void *p, size_t n) {
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* Build a valid module image with 2 consts (INT 42, STRING "hi") and 1 func. */
static void build_module(buf *b) {
    b->len = 0;

    /* Header (20 bytes). */
    put_u32(b, VL_MAGIC);   /* magic       */
    put_u16(b, 1);          /* version     */
    put_u16(b, 0);          /* flags       */
    put_u32(b, 2);          /* const_count */
    put_u32(b, 1);          /* func_count  */
    put_u32(b, 0);          /* entry       */

    /* Constant 0: INT = 42. */
    put_u8(b, QK_INT);
    put_u64(b, (uint64_t)42);

    /* Constant 1: STRING = "hi". */
    put_u8(b, QK_STRING);
    put_varint(b, 2);
    put_bytes(b, "hi", 2);

    /* Function 0: arity 0, num_locals 1, num_upvals 0, code [PUSHINT 7, RET]. */
    put_u8(b, 0);           /* arity      */
    put_u16(b, 1);          /* num_locals */
    put_u8(b, 0);           /* num_upvals */
    put_u32(b, 6);          /* code_len   */
    put_u8(b, OP_PUSHINT);
    put_u32(b, 7);          /* i32 immediate */
    put_u8(b, OP_RET);
}

/* Load a hand-built module and verify every field round-trips. */
static void test_load_valid(void) {
    buf b;
    build_module(&b);

    vl_module *m = NULL;
    CHECK_OK(vl_module_load(b.data, b.len, &m));
    CHECK(m != NULL);
    if (m == NULL) {
        return;
    }

    CHECK_EQ(m->version, 1);
    CHECK_EQ(m->flags, 0);
    CHECK_EQ(m->const_count, 2);
    CHECK_EQ(m->func_count, 1);
    CHECK_EQ(m->entry, 0);

    /* Constant 0: INT = 42. */
    const vl_const *c0 = vl_module_const(m, 0);
    CHECK(c0 != NULL);
    if (c0 != NULL) {
        CHECK_EQ(c0->kind, QK_INT);
        CHECK_EQ(c0->as.i, 42);
    }

    /* Constant 1: STRING "hi". */
    const vl_const *c1 = vl_module_const(m, 1);
    CHECK(c1 != NULL);
    if (c1 != NULL) {
        CHECK_EQ(c1->kind, QK_STRING);
        CHECK_EQ(c1->as.s.len, 2);
        CHECK(c1->as.s.data != NULL);
        if (c1->as.s.data != NULL) {
            CHECK_EQ(memcmp(c1->as.s.data, "hi", 2), 0);
            CHECK_EQ(c1->as.s.data[2], '\0'); /* NUL-terminated per module.h */
        }
    }

    /* Out-of-range constant index is bounds-checked. */
    CHECK(vl_module_const(m, 2) == NULL);

    /* Function 0. */
    const vl_function *f0 = vl_module_func(m, 0);
    CHECK(f0 != NULL);
    if (f0 != NULL) {
        CHECK_EQ(f0->arity, 0);
        CHECK_EQ(f0->num_locals, 1);
        CHECK_EQ(f0->num_upvals, 0);
        CHECK_EQ(f0->code_len, 6);
        CHECK(f0->code != NULL);
        if (f0->code != NULL) {
            CHECK_EQ(f0->code[0], OP_PUSHINT);
            CHECK_EQ(f0->code[5], OP_RET);
        }
    }

    /* Out-of-range function index is bounds-checked. */
    CHECK(vl_module_func(m, 1) == NULL);

    vl_module_free(m);
}

/* A truncated image must be rejected, not crash. */
static void test_truncated(void) {
    buf b;
    build_module(&b);

    vl_module *m = NULL;
    vl_status s = vl_module_load(b.data, 10, &m); /* first 10 bytes only */
    CHECK(s != VL_OK);
    CHECK(m == NULL);
    if (m != NULL) {
        vl_module_free(m);
    }
}

/* A wrong magic must return VL_ERR_BAD_MAGIC. */
static void test_bad_magic(void) {
    buf b;
    build_module(&b);
    b.data[0] ^= 0xFF; /* corrupt the first magic byte */

    vl_module *m = NULL;
    vl_status s = vl_module_load(b.data, b.len, &m);
    CHECK_EQ(s, VL_ERR_BAD_MAGIC);
    CHECK(m == NULL);
    if (m != NULL) {
        vl_module_free(m);
    }
}

/* Run all module loader tests. */
int main(void) {
    test_load_valid();
    test_truncated();
    test_bad_magic();
    return TEST_SUMMARY();
}
