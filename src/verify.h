/*
 * verify.h - static bytecode verification.
 *
 * Before a module runs, the verifier walks every function's bytecode once and
 * checks that: opcodes are known and fully contained; operands index valid
 * constants, locals, upvalues, and functions; jumps land on instruction
 * boundaries within the function; and the operand stack neither underflows nor
 * exceeds a computed bound along any path. A module that fails verification is
 * never executed.
 */
#ifndef VELLUM_VERIFY_H
#define VELLUM_VERIFY_H

#include "internal.h"
#include "module.h"

/* Verify every function in the module. Returns VL_OK or VL_ERR_BAD_CODE. */
vl_status vl_verify_module(const vl_module *m);

/*
 * Verify a single function and, on success, store the maximum operand-stack
 * depth it can reach in *max_stack (used by the VM to size frames).
 */
vl_status vl_verify_function(const vl_module *m, const vl_function *f,
                             uint32_t *max_stack);

#endif /* VELLUM_VERIFY_H */
