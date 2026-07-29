/*
 * optimize.h - a size-preserving peephole optimizer for loaded modules.
 *
 * Rewrites provably-redundant instruction sequences (a push immediately
 * discarded by a pop; a doubled negation or logical-not) into NOPs. Because it
 * never changes code length, jump offsets stay valid and the module remains
 * verifiable. Intended for the qopt tool and offline analysis, not the fuzzing
 * path.
 */
#ifndef VELLUM_OPTIMIZE_H
#define VELLUM_OPTIMIZE_H

#include "internal.h"
#include "module.h"

/*
 * Peephole-optimize every function in the module in place. On return *removed
 * (if non-NULL) holds the number of instructions neutralized to NOPs.
 */
vl_status vl_optimize_module(vl_module *m, uint32_t *removed);

#endif /* VELLUM_OPTIMIZE_H */
