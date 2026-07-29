/*
 * test_optimize.c - unit tests for the peephole optimizer (optimize.h).
 *
 * Each case assembles a single-function module with the programmatic builder,
 * loads it, and runs vl_optimize_module. The optimizer is size-preserving: a
 * redundant push/pop pair is rewritten to two NOPs, leaving jump offsets and
 * the code length intact so the module still verifies afterward.
 */
#include "builder.h"
#include "module.h"
#include "opcodes.h"
#include "optimize.h"
#include "verify.h"

#include "harness.h"

/*
 * Build a single-function entry module from a caller-populated builder, finish
 * it to bytes, and load it. Returns the loaded module or NULL on any failure;
 * the builder is always freed.
 */
static vl_module *finish_and_load(vl_builder *b, int func) {
    uint8_t *bytes = NULL;
    size_t len = 0;
    vl_module *m = NULL;

    vl_builder_set_entry(b, func);
    if (vl_builder_finish(b, &bytes, &len) != VL_OK) {
        vl_builder_free(b);
        return NULL;
    }
    vl_builder_free(b);
    if (!bytes) {
        return NULL;
    }
    if (vl_module_load(bytes, len, &m) != VL_OK) {
        m = NULL;
    }
    vl_free(bytes);
    return m;
}

/*
 * [PUSHINT 5, POP, PUSHINT 9, RET]: the leading push is immediately discarded,
 * so both instructions are neutralized to NOPs (removed == 2) and the module
 * still verifies.
 */
static void test_redundant_pair(void) {
    vl_builder *b = vl_builder_new();
    CHECK(b != NULL);
    if (!b) {
        return;
    }

    int func = vl_builder_begin_func(b, 0, 0);
    CHECK(func >= 0);
    CHECK_OK(vl_builder_emit_i32(b, func, OP_PUSHINT, 5));
    CHECK_OK(vl_builder_emit(b, func, OP_POP));
    CHECK_OK(vl_builder_emit_i32(b, func, OP_PUSHINT, 9));
    CHECK_OK(vl_builder_emit(b, func, OP_RET));

    vl_module *m = finish_and_load(b, func);
    CHECK(m != NULL);
    if (m) {
        uint32_t removed = 0;
        CHECK_OK(vl_optimize_module(m, &removed));
        CHECK_EQ(removed, 2);
        CHECK_OK(vl_verify_module(m));
        CHECK(m->func_count >= 1);
        if (m->func_count >= 1) {
            CHECK_EQ(m->funcs[0].code[0], OP_NOP);
        }
        vl_module_free(m);
    }
}

/*
 * [PUSHINT 9, RET] holds no redundant sequence, so nothing is neutralized
 * (removed == 0).
 */
static void test_no_redundant_pair(void) {
    vl_builder *b = vl_builder_new();
    CHECK(b != NULL);
    if (!b) {
        return;
    }

    int func = vl_builder_begin_func(b, 0, 0);
    CHECK(func >= 0);
    CHECK_OK(vl_builder_emit_i32(b, func, OP_PUSHINT, 9));
    CHECK_OK(vl_builder_emit(b, func, OP_RET));

    vl_module *m = finish_and_load(b, func);
    CHECK(m != NULL);
    if (m) {
        uint32_t removed = 99;
        CHECK_OK(vl_optimize_module(m, &removed));
        CHECK_EQ(removed, 0);
        CHECK_OK(vl_verify_module(m));
        vl_module_free(m);
    }
}

int main(void) {
    test_redundant_pair();
    test_no_redundant_pair();
    return TEST_SUMMARY();
}
