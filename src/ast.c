/*
 * ast.c - destruction of Vellum source-language syntax trees.
 *
 * The parser builds a tree of heap-allocated vl_expr / vl_stmt nodes rooted in
 * a vl_program of function definitions. This file implements the single public
 * entry point vl_ast_free_program, which recursively releases every node,
 * owned string, and child-pointer array that a program owns. The recursion is
 * split into two static helpers, free_expr and free_stmt, mirroring the two
 * node families. Every function here is NULL-safe so partially built trees
 * from a failed parse can be torn down without special casing.
 */
#include "ast.h"

static void free_expr(vl_expr *e);
static void free_stmt(vl_stmt *s);

/*
 * free_expr - recursively free an expression node and everything it owns.
 * Frees child expressions, owned strings, and child-pointer arrays before the
 * node itself. NULL-safe.
 */
static void free_expr(vl_expr *e) {
    int i;

    if (e == NULL) {
        return;
    }
    switch (e->kind) {
    case EX_NIL:
    case EX_BOOL:
    case EX_INT:
    case EX_REAL:
        break;
    case EX_STR:
        vl_free(e->as.str.data);
        break;
    case EX_IDENT:
        vl_free(e->as.ident.name);
        break;
    case EX_UNARY:
        free_expr(e->as.unary.e);
        break;
    case EX_BINARY:
        free_expr(e->as.binary.l);
        free_expr(e->as.binary.r);
        break;
    case EX_CALL:
        free_expr(e->as.call.callee);
        if (e->as.call.args != NULL) {
            for (i = 0; i < e->as.call.nargs; i++) {
                free_expr(e->as.call.args[i]);
            }
            vl_free(e->as.call.args);
        }
        break;
    case EX_INDEX:
        free_expr(e->as.index.base);
        free_expr(e->as.index.index);
        break;
    case EX_ARRAY:
        if (e->as.array.items != NULL) {
            for (i = 0; i < e->as.array.n; i++) {
                free_expr(e->as.array.items[i]);
            }
            vl_free(e->as.array.items);
        }
        break;
    case EX_MAP:
        if (e->as.map.keys != NULL) {
            for (i = 0; i < e->as.map.n; i++) {
                free_expr(e->as.map.keys[i]);
            }
            vl_free(e->as.map.keys);
        }
        if (e->as.map.vals != NULL) {
            for (i = 0; i < e->as.map.n; i++) {
                free_expr(e->as.map.vals[i]);
            }
            vl_free(e->as.map.vals);
        }
        break;
    }
    vl_free(e);
}

/*
 * free_stmt - recursively free a statement node and everything it owns.
 * Frees child statements, child expressions, owned strings, and child-pointer
 * arrays before the node itself. NULL-safe.
 */
static void free_stmt(vl_stmt *s) {
    int i;

    if (s == NULL) {
        return;
    }
    switch (s->kind) {
    case ST_LET:
        vl_free(s->as.let.name);
        free_expr(s->as.let.init);
        break;
    case ST_ASSIGN:
        free_expr(s->as.assign.target);
        free_expr(s->as.assign.value);
        break;
    case ST_IF:
        free_expr(s->as.if_s.cond);
        free_stmt(s->as.if_s.then_s);
        free_stmt(s->as.if_s.else_s);
        break;
    case ST_WHILE:
        free_expr(s->as.while_s.cond);
        free_stmt(s->as.while_s.body);
        break;
    case ST_RETURN:
        free_expr(s->as.ret.value);
        break;
    case ST_PRINT:
        free_expr(s->as.print.value);
        break;
    case ST_EXPR:
        free_expr(s->as.expr.e);
        break;
    case ST_BLOCK:
        if (s->as.block.items != NULL) {
            for (i = 0; i < s->as.block.n; i++) {
                free_stmt(s->as.block.items[i]);
            }
            vl_free(s->as.block.items);
        }
        break;
    }
    vl_free(s);
}

/*
 * free_func_def - free a single function definition: its name, each parameter
 * string and the parameter array, and its body block. NULL-safe.
 */
static void free_func_def(vl_func_def *f) {
    int i;

    if (f == NULL) {
        return;
    }
    vl_free(f->name);
    if (f->params != NULL) {
        for (i = 0; i < f->nparams; i++) {
            vl_free(f->params[i]);
        }
        vl_free(f->params);
    }
    free_stmt(f->body);
    vl_free(f);
}

/*
 * vl_ast_free_program - recursively free a program and everything it owns:
 * every function definition, then the function pointer array, then the program
 * struct itself. NULL-safe.
 */
void vl_ast_free_program(vl_program *p) {
    int i;

    if (p == NULL) {
        return;
    }
    if (p->funcs != NULL) {
        for (i = 0; i < p->nfuncs; i++) {
            free_func_def(p->funcs[i]);
        }
        vl_free(p->funcs);
    }
    vl_free(p);
}
