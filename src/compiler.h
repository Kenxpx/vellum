/*
 * compiler.h - compile a parsed Vellum program to a .qbc module.
 *
 * Each source function becomes a VM function: parameters occupy the first local
 * slots, `let` bindings allocate further slots, control flow lowers to jumps
 * with backpatched offsets, and calls resolve a function name to its index and
 * emit CLOSURE/CALL. The function named "main" (arity 0) is the module entry.
 * The emitted module always passes the verifier.
 */
#ifndef VELLUM_COMPILER_H
#define VELLUM_COMPILER_H

#include "ast.h"
#include "internal.h"

/*
 * Compile a program to a freshly allocated .qbc buffer (*out,*out_len). On a
 * compile error (unknown name, too many locals, missing main, ...) returns a
 * non-OK status and writes a short message into err[0,errcap) when err != NULL.
 */
vl_status vl_compile(const vl_program *p, uint8_t **out, size_t *out_len,
                     char *err, size_t errcap);

/* Convenience: parse then compile source text in one call. */
vl_status vl_compile_source(const char *src, size_t len, uint8_t **out,
                            size_t *out_len, char *err, size_t errcap);

#endif /* VELLUM_COMPILER_H */
