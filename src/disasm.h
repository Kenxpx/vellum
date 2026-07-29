/*
 * disasm.h - human-readable disassembly of a module, used by the qdis tool and
 * for diagnostics. Renders the constant pool and every function's instructions
 * with resolved operands.
 */
#ifndef VELLUM_DISASM_H
#define VELLUM_DISASM_H

#include <stdio.h>

#include "internal.h"
#include "module.h"

/* Disassemble the whole module (header, constants, functions) to out. */
vl_status vl_disasm_module(const vl_module *m, FILE *out);

/* Disassemble one function's bytecode to out. */
vl_status vl_disasm_function(const vl_module *m, const vl_function *f, FILE *out);

#endif /* VELLUM_DISASM_H */
