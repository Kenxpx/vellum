/*
 * test_compile.c - end-to-end tests for the Vellum source front end.
 *
 * Each case feeds a small source program (with an `fn main` returning an int)
 * through the whole pipeline: vl_compile_source produces a .qbc byte buffer,
 * vl_module_load structurally validates it, vl_verify_module checks the
 * bytecode, and vl_run_module executes the entry function. The returned value
 * is checked against the expected integer. Every program is benign: pure
 * integer arithmetic, a plain call, an if/else, a counted while loop, and a
 * small array literal - no maps, closures, or unbounded recursion.
 */
#include "compiler.h"
#include "module.h"
#include "obj.h"
#include "value.h"
#include "verify.h"
#include "vm.h"

#include "harness.h"

/*
 * Compile src, load and verify the module, run its entry function, and check
 * that the result is the integer `expect`. CHECKs each stage (compile, load,
 * verify, run) and releases the result value plus the module and byte buffer
 * before returning; nothing is leaked on any early-exit path.
 */
static void check_program(const char *src, int64_t expect)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    vl_module *m = NULL;
    vl_value r = vl_nil();
    char err[256];
    vl_status st;

    err[0] = '\0';
    st = vl_compile_source(src, strlen(src), &buf, &len, err, sizeof(err));
    CHECK_EQ(st, VL_OK);
    if (st != VL_OK) {
        fprintf(stderr, "  compile error: %s\n", err);
        return;
    }

    st = vl_module_load(buf, len, &m);
    CHECK_EQ(st, VL_OK);
    if (st != VL_OK) {
        vl_free(buf);
        return;
    }

    st = vl_verify_module(m);
    CHECK_EQ(st, VL_OK);
    if (st == VL_OK) {
        st = vl_run_module(m, NULL, &r);
        CHECK_EQ(st, VL_OK);
    }

    CHECK_EQ(r.tag, VL_INT);
    CHECK_EQ(r.as.i, expect);

    vl_val_release(r);
    vl_module_free(m);
    vl_free(buf);
}

/* (a) constant-folding-free arithmetic: 3 * 4 + 2 evaluates to 14. */
static void test_arith(void)
{
    check_program("fn main(){ return 3*4+2; }", 14);
}

/* (b) a two-argument call: add(20, 22) returns 42. */
static void test_call(void)
{
    check_program("fn add(a,b){ return a+b; } fn main(){ return add(20,22); }",
                  42);
}

/* (c) if/else selects the true branch when the condition holds. */
static void test_if_else(void)
{
    check_program(
        "fn main(){ let x = 5; if (x > 3) { return 7; } else { return 9; } }",
        7);
}

/* (d) a while loop with let/assignment sums 1..5 to 15. */
static void test_while_sum(void)
{
    check_program(
        "fn main(){"
        "  let sum = 0;"
        "  let i = 1;"
        "  while (i <= 5) {"
        "    sum = sum + i;"
        "    i = i + 1;"
        "  }"
        "  return sum;"
        "}",
        15);
}

/* (e) an array literal is indexed: a[1] + a[2] == 20 + 30 == 50. */
static void test_array_index(void)
{
    check_program("fn main(){ let a=[10,20,30]; return a[1]+a[2]; }", 50);
}

int main(void)
{
    test_arith();
    test_call();
    test_if_else();
    test_while_sum();
    test_array_index();
    return TEST_SUMMARY();
}
