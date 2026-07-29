/*
 * test_value.c - unit tests for value.h: exercise vl_value_truthy across the
 * immediate tags and vl_value_equal for equal/unequal values and tag mismatches.
 */
#include "harness.h"
#include "value.h"

/* Verify truthiness: only nil and false are falsey; all other immediates truthy. */
static void test_truthy(void) {
    CHECK_EQ(vl_value_truthy(vl_nil()), 0);
    CHECK_EQ(vl_value_truthy(vl_bool(0)), 0);
    CHECK_EQ(vl_value_truthy(vl_bool(1)), 1);
    CHECK_EQ(vl_value_truthy(vl_int(0)), 1);
    CHECK_EQ(vl_value_truthy(vl_int(5)), 1);
    CHECK_EQ(vl_value_truthy(vl_real(3.14)), 1);
}

/* Verify equality: same tag+value equal; differing tags or values not equal. */
static void test_equal(void) {
    CHECK_EQ(vl_value_equal(vl_int(5), vl_int(5)), 1);
    CHECK_EQ(vl_value_equal(vl_int(5), vl_int(7)), 0);
    CHECK_EQ(vl_value_equal(vl_int(5), vl_real(5.0)), 0);
    CHECK_EQ(vl_value_equal(vl_nil(), vl_nil()), 1);
    CHECK_EQ(vl_value_equal(vl_bool(1), vl_bool(1)), 1);
    CHECK_EQ(vl_value_equal(vl_bool(0), vl_int(0)), 0);
}

/* Run the value tests and report a pass/fail summary. */
int main(void) {
    test_truthy();
    test_equal();
    return TEST_SUMMARY();
}
