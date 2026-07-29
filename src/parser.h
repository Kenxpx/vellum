/*
 * parser.h - a recursive-descent parser for the Vellum source language.
 *
 * Grammar (see docs/LANGUAGE.md): a program is one or more function
 * definitions `fn name(params) { ... }`. Statements are let/assignment/if/
 * while/return/print/expression/block. Expressions use the usual precedence
 * (or, and, comparison, additive, multiplicative, unary, call/index, primary),
 * with int/real/string/bool/nil literals, identifiers, array `[...]` and map
 * `{k: v}` literals, calls, and indexing.
 */
#ifndef VELLUM_PARSER_H
#define VELLUM_PARSER_H

#include "ast.h"
#include "internal.h"

/*
 * Parse src[0,len) into a program. On success stores the tree in *out (caller
 * frees with vl_ast_free_program). On a syntax error returns a non-OK status
 * and, if err is non-NULL, writes a short "line N: ..." message into err[0,cap).
 */
vl_status vl_parse(const char *src, size_t len, vl_program **out, char *err,
                   size_t errcap);

#endif /* VELLUM_PARSER_H */
