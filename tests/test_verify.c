/*
 * test_verify.c - unit tests for the static bytecode verifier (verify.h).
 *
 * Each case assembles a full single-function module in memory (const_count 0,
 * func_count 1, entry 0), loads it with vl_module_load, then runs the verifier
 * and checks the outcome: a well-formed function verifies and reports the right
 * max_stack; malformed ones are rejected with VL_ERR_BAD_CODE.
 */
#include "internal.h"
#include "module.h"
#include "opcodes.h"
#include "verify.h"
#include "harness.h"

/*
 * Assemble a module with a single function holding the given code and load it.
 * The function has arity 0, no locals, no upvalues; the module has no
 * constants. Returns the vl_module_load status; *out is set on success.
 */
static vl_status build_and_load(const uint8_t *code, uint32_t code_len,
                                vl_module **out)
{
    uint8_t buf[256];
    size_t off;

    /* Header (20 bytes). */
    vl_store_u32le(buf + 0, VL_MAGIC);
    vl_store_u16le(buf + 4, VL_VERSION);
    vl_store_u16le(buf + 6, 0);  /* flags       */
    vl_store_u32le(buf + 8, 0);  /* const_count */
    vl_store_u32le(buf + 12, 1); /* func_count  */
    vl_store_u32le(buf + 16, 0); /* entry       */
    off = 20;

    /* Single function descriptor. */
    buf[off++] = 0;                     /* arity      */
    vl_store_u16le(buf + off, 0);       /* num_locals */
    off += 2;
    buf[off++] = 0;                     /* num_upvals */
    vl_store_u32le(buf + off, code_len);/* code_len   */
    off += 4;
    memcpy(buf + off, code, code_len);
    off += code_len;

    return vl_module_load(buf, off, out);
}

/* A well-formed [PUSHINT 7, PUSHINT 8, ADD, RET] verifies with max_stack 2. */
static void test_wellformed(void)
{
    uint8_t code[12];
    size_t n = 0;
    vl_module *m = NULL;
    const vl_function *f;
    uint32_t max_stack = 0;

    code[n++] = OP_PUSHINT;
    vl_store_u32le(code + n, 7);
    n += 4;
    code[n++] = OP_PUSHINT;
    vl_store_u32le(code + n, 8);
    n += 4;
    code[n++] = OP_ADD;
    code[n++] = OP_RET;

    CHECK_OK(build_and_load(code, (uint32_t)n, &m));
    CHECK(m != NULL);
    if (m) {
        CHECK_OK(vl_verify_module(m));
        f = vl_module_func(m, 0);
        CHECK(f != NULL);
        if (f) {
            CHECK_OK(vl_verify_function(m, f, &max_stack));
            CHECK_EQ(max_stack, 2);
        }
        vl_module_free(m);
    }
}

/* A trailing PUSHINT whose i32 operand runs past code_len is rejected. */
static void test_operand_past_end(void)
{
    /* PUSHINT needs 4 operand bytes but only 2 follow. */
    uint8_t code[3] = { OP_PUSHINT, 0x00, 0x00 };
    vl_module *m = NULL;

    CHECK_OK(build_and_load(code, (uint32_t)sizeof(code), &m));
    CHECK(m != NULL);
    if (m) {
        CHECK_EQ(vl_verify_module(m), VL_ERR_BAD_CODE);
        vl_module_free(m);
    }
}

/* RET on an empty operand stack underflows and is rejected. */
static void test_stack_underflow(void)
{
    uint8_t code[1] = { OP_RET };
    vl_module *m = NULL;

    CHECK_OK(build_and_load(code, (uint32_t)sizeof(code), &m));
    CHECK(m != NULL);
    if (m) {
        CHECK_EQ(vl_verify_module(m), VL_ERR_BAD_CODE);
        vl_module_free(m);
    }
}

/* PUSHK with a constant index >= const_count is rejected. */
static void test_bad_const_index(void)
{
    /* const_count is 0, so kidx 0 is out of range. */
    uint8_t code[3];
    vl_module *m = NULL;

    code[0] = OP_PUSHK;
    vl_store_u16le(code + 1, 0);

    CHECK_OK(build_and_load(code, (uint32_t)sizeof(code), &m));
    CHECK(m != NULL);
    if (m) {
        CHECK_EQ(vl_verify_module(m), VL_ERR_BAD_CODE);
        vl_module_free(m);
    }
}

int main(void)
{
    test_wellformed();
    test_operand_past_end();
    test_stack_underflow();
    test_bad_const_index();
    return TEST_SUMMARY();
}
