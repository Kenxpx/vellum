/*
 * asm.h - a small text assembler for Vellum.
 *
 * Parses a line-oriented assembly source (directives for constants, functions,
 * and upvalues; one instruction per line; labels for jumps) into a .qbc module
 * using the builder. Used by the qasm tool; not on the fuzzing path.
 *
 * Grammar (one item per line, '#' starts a comment):
 *   .const int <n> | .const real <x> | .const string "<text>"
 *   .func <arity> <locals>        ; begins a function body
 *   .upval local <i> | .upval upval <i>
 *   .entry <func-index>
 *   <label>:                      ; a jump target
 *   <mnemonic> [operand]          ; e.g.  pushint 7 / call 1 / jmp <label>
 */
#ifndef VELLUM_ASM_H
#define VELLUM_ASM_H

#include "internal.h"

/*
 * Assemble text[0,len) into a freshly allocated .qbc buffer (*out,*out_len).
 * On error returns a non-OK status and, if errbuf is non-NULL, writes a short
 * message (with the offending line number) into errbuf[0,errcap).
 */
vl_status vl_assemble(const char *text, size_t len, uint8_t **out,
                      size_t *out_len, char *errbuf, size_t errcap);

#endif /* VELLUM_ASM_H */
