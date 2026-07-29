/*
 * test_builder.c - build a tiny module with the programmatic builder, load,
 * verify, and run it, checking that 7 + 8 evaluates to the integer 15.
 */
#include "builder.h"
#include "module.h"
#include "obj.h"
#include "opcodes.h"
#include "value.h"
#include "verify.h"
#include "vm.h"

#include "harness.h"

/* Assemble the entry function, run it, and check the returned value. */
static void test_add(void) {
    vl_builder *b = vl_builder_new();
    CHECK(b != NULL);
    if (!b) {
        return;
    }

    int func = vl_builder_begin_func(b, 0, 0);
    CHECK(func >= 0);
    CHECK_OK(vl_builder_emit_i32(b, func, OP_PUSHINT, 7));
    CHECK_OK(vl_builder_emit_i32(b, func, OP_PUSHINT, 8));
    CHECK_OK(vl_builder_emit(b, func, OP_ADD));
    CHECK_OK(vl_builder_emit(b, func, OP_RET));
    vl_builder_set_entry(b, func);

    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK_OK(vl_builder_finish(b, &bytes, &len));
    vl_builder_free(b);
    CHECK(bytes != NULL);
    if (!bytes) {
        return;
    }

    vl_module *m = NULL;
    CHECK_OK(vl_module_load(bytes, len, &m));
    CHECK(m != NULL);
    if (m) {
        CHECK_OK(vl_verify_module(m));

        vl_value result = vl_nil();
        CHECK_OK(vl_run_module(m, NULL, &result));
        CHECK_EQ(result.tag, VL_INT);
        CHECK_EQ(result.as.i, 15);
        vl_val_release(result);

        vl_module_free(m);
    }

    vl_free(bytes);
}

int main(void) {
    test_add();
    return TEST_SUMMARY();
}
