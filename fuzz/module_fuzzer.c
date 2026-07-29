/*
 * module_fuzzer.c - drive the whole VM: load a module, verify it, and run it.
 * This is the deepest path through Vellum (loader -> verifier -> interpreter,
 * with the object heap, closures, and call frames).
 */
#include <stddef.h>
#include <stdint.h>

#include "vm.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    vl_exec(data, size, NULL);
    return 0;
}
