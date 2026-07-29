/*
 * harness.h - a minimal, dependency-free unit-test scaffold.
 *
 * Each test file is its own executable: include this header, write checks with
 * CHECK / CHECK_EQ, and return TEST_SUMMARY() from main. The counters are
 * file-local, so one translation unit maps to one test program.
 */
#ifndef VELLUM_TEST_HARNESS_H
#define VELLUM_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int vl_t_run = 0;
static int vl_t_fail = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        vl_t_run++;                                                          \
        if (!(cond)) {                                                       \
            vl_t_fail++;                                                     \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long long vl_va = (long long)(a);                                    \
        long long vl_vb = (long long)(b);                                    \
        vl_t_run++;                                                          \
        if (vl_va != vl_vb) {                                                \
            vl_t_fail++;                                                     \
            fprintf(stderr, "  FAIL %s:%d: %s (%lld) != %s (%lld)\n",        \
                    __FILE__, __LINE__, #a, vl_va, #b, vl_vb);               \
        }                                                                    \
    } while (0)

#define CHECK_OK(expr) CHECK_EQ((expr), VL_OK)

#define TEST_SUMMARY()                                                       \
    (fprintf(stderr, "%s: %d/%d checks passed\n", __FILE__,                  \
             vl_t_run - vl_t_fail, vl_t_run),                                \
     vl_t_fail == 0 ? 0 : 1)

#endif /* VELLUM_TEST_HARNESS_H */
