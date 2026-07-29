/*
 * verify_fuzzer.c - drive the module loader and static bytecode verifier
 * without executing. Exercises header/constant/function parsing and the
 * verifier's operand, jump-target, and stack-depth checks.
 */
#include <stddef.h>
#include <stdint.h>

#include "module.h"
#include "verify.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    vl_module *m = NULL;
    if (vl_module_load(data, size, &m) == VL_OK) {
        vl_verify_module(m);
        vl_module_free(m);
    }
    return 0;
}
