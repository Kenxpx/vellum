/*
 * test_asm.c - unit tests for the text assembler (asm.h).
 *
 * Each case runs vl_assemble on a small source program, then loads, verifies,
 * and runs the resulting module and checks the integer the program computes.
 * The first program stores to and reads back a local; the second uses a label
 * and a backward jump to sum a short range. A failed assemble is fatal and the
 * assembler's error buffer is printed for diagnosis.
 */
#include "internal.h"

#include "asm.h"
#include "module.h"
#include "obj.h"
#include "verify.h"
#include "vm.h"

#include "harness.h"

/*
 * Assemble src, load/verify/run it, and check the result is integer 'want'.
 * Any assemble/load/verify/run failure is reported (with the assembler's error
 * message on an assemble failure) and counts as a failed check.
 */
static void run_expect_int(const char *src, int64_t want)
{
    char errbuf[256];
    uint8_t *bytes = NULL;
    size_t len = 0;
    vl_status s;
    vl_module *m = NULL;
    vl_value result = vl_nil();

    errbuf[0] = '\0';
    s = vl_assemble(src, strlen(src), &bytes, &len, errbuf, sizeof(errbuf));
    CHECK_OK(s);
    if (s != VL_OK) {
        fprintf(stderr, "  assemble failed: %s\n", errbuf);
        return;
    }
    CHECK(bytes != NULL);

    if (bytes != NULL) {
        CHECK_OK(vl_module_load(bytes, len, &m));
        CHECK(m != NULL);
        if (m != NULL) {
            CHECK_OK(vl_verify_module(m));
            CHECK_OK(vl_run_module(m, NULL, &result));
            CHECK_EQ(result.tag, VL_INT);
            if (result.tag == VL_INT) {
                CHECK_EQ(result.as.i, want);
            }
            vl_val_release(result);
            vl_module_free(m);
        }
        vl_free(bytes);
    }
}

/* A store/load round-trip: local 0 = 10, then 10 + 5 = 15. */
static void test_local_roundtrip(void)
{
    static const char src[] =
        ".func 0 1\n"
        "pushint 10\n"
        "storelocal 0\n"
        "loadlocal 0\n"
        "pushint 5\n"
        "add\n"
        "ret\n"
        ".entry 0\n";

    run_expect_int(src, 15);
}

/* A label and a backward jmp summing 1..5, which is 15. */
static void test_label_loop(void)
{
    /* local 0 = sum, local 1 = counter i. Loop while i <= 5. */
    static const char src[] =
        ".func 0 2\n"
        "pushint 0\n"
        "storelocal 0\n"      /* sum = 0        */
        "pushint 1\n"
        "storelocal 1\n"      /* i = 1          */
        "loop:\n"
        "loadlocal 1\n"
        "pushint 5\n"
        "gt\n"                /* i > 5 ?        */
        "jmpif done\n"        /* if so, finish  */
        "loadlocal 0\n"
        "loadlocal 1\n"
        "add\n"
        "storelocal 0\n"      /* sum += i       */
        "loadlocal 1\n"
        "pushint 1\n"
        "add\n"
        "storelocal 1\n"      /* i += 1         */
        "jmp loop\n"
        "done:\n"
        "loadlocal 0\n"
        "ret\n"
        ".entry 0\n";

    run_expect_int(src, 15);
}

int main(void)
{
    test_local_roundtrip();
    test_label_loop();
    return TEST_SUMMARY();
}
