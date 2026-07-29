/*
 * vm.h - the Vellum virtual machine.
 *
 * Runs a loaded, verified module's entry function on a value stack with call
 * frames, closures, and a reference-counted object heap. Execution is bounded
 * by an instruction budget and stack/frame caps so that any well-formed module
 * terminates. The VM owns everything it allocates and tears it down on return.
 */
#ifndef VELLUM_VM_H
#define VELLUM_VM_H

#include "internal.h"
#include "module.h"
#include "obj.h"
#include "value.h"

typedef struct vl_vm_limits {
    uint64_t instr_budget; /* max instructions before VL_ERR_BUDGET */
    uint32_t max_stack;    /* max value-stack depth                 */
    uint32_t max_frames;   /* max call depth                        */
} vl_vm_limits;

/* Fill l with default limits (bounded, suitable for fuzzing). */
void vl_vm_limits_default(vl_vm_limits *l);

/*
 * Run module m's entry function with the given limits (NULL for defaults). On a
 * normal HALT/RET the top value is copied into *result (if non-NULL) as a
 * detached, still-referenced value the caller must release with
 * vl_val_release; on any error *result is set to nil.
 */
vl_status vl_run_module(const vl_module *m, const vl_vm_limits *limits,
                        vl_value *result);

/*
 * Convenience entry point: load a module from raw bytes, verify it, run it, and
 * free it. This is the single call a host (or a fuzz harness) needs. Any
 * *result is released internally; pass NULL. Returns the first failing status.
 */
vl_status vl_exec(const uint8_t *data, size_t size, const vl_vm_limits *limits);

#endif /* VELLUM_VM_H */
