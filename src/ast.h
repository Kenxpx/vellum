/*
 * ast.h - the abstract syntax tree for the Vellum source language.
 *
 * The parser (parser.h) builds a program: a list of function definitions, each
 * a name, parameter list, and a body block of statements over an expression
 * grammar. The compiler (compiler.h) walks this tree and emits bytecode. Nodes
 * are heap-allocated; vl_ast_free_program releases a whole tree.
 */
#ifndef VELLUM_AST_H
#define VELLUM_AST_H

#include "internal.h"

typedef struct vl_expr vl_expr;
typedef struct vl_stmt vl_stmt;

typedef enum vl_expr_kind {
    EX_NIL, EX_BOOL, EX_INT, EX_REAL, EX_STR, EX_IDENT,
    EX_UNARY, EX_BINARY, EX_CALL, EX_INDEX, EX_ARRAY, EX_MAP
} vl_expr_kind;

/* Operator codes for unary/binary nodes (parser-defined, compiler-consumed). */
typedef enum vl_op_kind {
    OPK_ADD, OPK_SUB, OPK_MUL, OPK_DIV, OPK_MOD,
    OPK_EQ, OPK_NE, OPK_LT, OPK_LE, OPK_GT, OPK_GE,
    OPK_AND, OPK_OR, OPK_NEG, OPK_NOT
} vl_op_kind;

struct vl_expr {
    vl_expr_kind kind;
    int line;
    union {
        int b;                                  /* EX_BOOL */
        int64_t i;                              /* EX_INT  */
        double d;                               /* EX_REAL */
        struct { char *data; uint32_t len; } str;   /* EX_STR   */
        struct { char *name; } ident;                /* EX_IDENT */
        struct { int op; vl_expr *e; } unary;        /* EX_UNARY */
        struct { int op; vl_expr *l; vl_expr *r; } binary; /* EX_BINARY */
        struct { vl_expr *callee; vl_expr **args; int nargs; } call; /* callee is EX_IDENT */
        struct { vl_expr *base; vl_expr *index; } index; /* EX_INDEX: base[index] */
        struct { vl_expr **items; int n; } array;    /* EX_ARRAY literal */
        struct { vl_expr **keys; vl_expr **vals; int n; } map; /* EX_MAP literal */
    } as;
};

typedef enum vl_stmt_kind {
    ST_LET, ST_ASSIGN, ST_IF, ST_WHILE, ST_RETURN, ST_PRINT, ST_EXPR, ST_BLOCK
} vl_stmt_kind;

struct vl_stmt {
    vl_stmt_kind kind;
    int line;
    union {
        struct { char *name; vl_expr *init; } let;         /* let name = init; */
        struct { vl_expr *target; vl_expr *value; } assign; /* target(IDENT|INDEX) = value; */
        struct { vl_expr *cond; vl_stmt *then_s; vl_stmt *else_s; } if_s; /* else_s may be NULL */
        struct { vl_expr *cond; vl_stmt *body; } while_s;
        struct { vl_expr *value; } ret;                     /* value may be NULL */
        struct { vl_expr *value; } print;
        struct { vl_expr *e; } expr;
        struct { vl_stmt **items; int n; } block;
    } as;
};

typedef struct vl_func_def {
    char *name;
    char **params;
    int nparams;
    vl_stmt *body; /* an ST_BLOCK */
} vl_func_def;

typedef struct vl_program {
    vl_func_def **funcs;
    int nfuncs;
} vl_program;

/* Recursively free a program and everything it owns. NULL-safe. */
void vl_ast_free_program(vl_program *p);

#endif /* VELLUM_AST_H */
