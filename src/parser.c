/*
 * parser.c - a recursive-descent parser for the Vellum source language.
 *
 * Consumes the token stream produced by the lexer (lexer.h) and builds the AST
 * declared in ast.h: a program is a sequence of function definitions, each with
 * a body block over the statement and expression grammar. The parser keeps the
 * current token plus a one-token lookahead and an error buffer; on the first
 * syntax or allocation error it records "line N: <reason>", unwinds while freeing
 * every partially built node, and reports a non-OK status. Expression precedence,
 * from lowest to highest, is: or, and, equality, comparison, additive,
 * multiplicative, unary, postfix (call/index), primary.
 */
#include "parser.h"

#include <limits.h>
#include <stdio.h>

#include "internal.h"
#include "lexer.h"

/* Parser state: the token cursor, its one-token lookahead, and error status. */
typedef struct {
    vl_lexer lx;       /* underlying tokenizer                              */
    vl_token cur;      /* current (not-yet-consumed) token                  */
    vl_token peek;     /* one-token lookahead following cur                 */
    char *err;         /* caller error buffer (may be NULL)                 */
    size_t errcap;     /* capacity of err                                   */
    int had_error;     /* set once the first error is recorded              */
    vl_status status;  /* status to report (BAD_MODULE for syntax, OOM ...) */
} parser;

/* Forward declarations for the mutually recursive grammar and free routines. */
static vl_expr *parse_expr(parser *p);
static vl_stmt *parse_stmt(parser *p);
static vl_stmt *parse_block(parser *p);
static void free_expr(vl_expr *e);
static void free_stmt(vl_stmt *s);

/* Record the first "line N: <msg>" syntax error and mark the parser failed. */
static void p_error(parser *p, int line, const char *msg) {
    if (p->had_error) {
        return;
    }
    p->had_error = 1;
    p->status = VL_ERR_BAD_MODULE;
    if (p->err != NULL && p->errcap > 0) {
        snprintf(p->err, p->errcap, "line %d: %s", line, msg);
    }
}

/* Record an out-of-memory failure at the current token's line (once). */
static void p_oom(parser *p) {
    if (p->had_error) {
        return;
    }
    p->had_error = 1;
    p->status = VL_ERR_OOM;
    if (p->err != NULL && p->errcap > 0) {
        snprintf(p->err, p->errcap, "line %d: out of memory", p->cur.line);
    }
}

/* Allocate and zero an expression node of the given kind, or NULL on OOM. */
static vl_expr *new_expr(parser *p, vl_expr_kind kind, int line) {
    vl_expr *e = vl_calloc(1, sizeof(*e));
    if (e == NULL) {
        p_oom(p);
        return NULL;
    }
    e->kind = kind;
    e->line = line;
    return e;
}

/* Allocate and zero a statement node of the given kind, or NULL on OOM. */
static vl_stmt *new_stmt(parser *p, vl_stmt_kind kind, int line) {
    vl_stmt *s = vl_calloc(1, sizeof(*s));
    if (s == NULL) {
        p_oom(p);
        return NULL;
    }
    s->kind = kind;
    s->line = line;
    return s;
}

/* Copy s[0,n) into a fresh NUL-terminated string, or NULL (marking OOM). */
static char *dup_str(parser *p, const char *s, uint32_t n) {
    size_t bytes;
    char *d;
    if (!vl_size_add(n, 1, &bytes)) {
        p_oom(p);
        return NULL;
    }
    d = vl_malloc(bytes);
    if (d == NULL) {
        p_oom(p);
        return NULL;
    }
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/*
 * Ensure a growable pointer array has room for one more element. Returns the
 * (possibly reallocated) buffer, or NULL on overflow / allocation failure.
 */
static void *ptr_vec_grow(void *arr, int count, int *cap, size_t elemsz) {
    if (count == *cap) {
        int ncap;
        size_t bytes;
        void *np;
        if (*cap > INT_MAX / 2) {
            return NULL;
        }
        ncap = *cap ? *cap * 2 : 4;
        if (!vl_size_mul((size_t)ncap, elemsz, &bytes)) {
            return NULL;
        }
        np = vl_realloc(arr, bytes);
        if (np == NULL) {
            return NULL;
        }
        *cap = ncap;
        return np;
    }
    return arr;
}

/* Free a list of n expression nodes and the array holding them. NULL-safe. */
static void free_expr_list(vl_expr **list, int n) {
    int i;
    for (i = 0; i < n; i++) {
        free_expr(list[i]);
    }
    vl_free(list);
}

/* Free a list of n statement nodes and the array holding them. NULL-safe. */
static void free_stmt_list(vl_stmt **list, int n) {
    int i;
    for (i = 0; i < n; i++) {
        free_stmt(list[i]);
    }
    vl_free(list);
}

/* Free a list of n heap strings and the array holding them. NULL-safe. */
static void free_str_list(char **list, int n) {
    int i;
    for (i = 0; i < n; i++) {
        vl_free(list[i]);
    }
    vl_free(list);
}

/* Recursively free an expression node and everything it owns. NULL-safe. */
static void free_expr(vl_expr *e) {
    if (e == NULL) {
        return;
    }
    switch (e->kind) {
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
        free_expr_list(e->as.call.args, e->as.call.nargs);
        break;
    case EX_INDEX:
        free_expr(e->as.index.base);
        free_expr(e->as.index.index);
        break;
    case EX_ARRAY:
        free_expr_list(e->as.array.items, e->as.array.n);
        break;
    case EX_MAP:
        free_expr_list(e->as.map.keys, e->as.map.n);
        free_expr_list(e->as.map.vals, e->as.map.n);
        break;
    case EX_NIL:
    case EX_BOOL:
    case EX_INT:
    case EX_REAL:
        break;
    }
    vl_free(e);
}

/* Recursively free a statement node and everything it owns. NULL-safe. */
static void free_stmt(vl_stmt *s) {
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
        free_stmt_list(s->as.block.items, s->as.block.n);
        break;
    }
    vl_free(s);
}

/* Free a function definition and everything it owns. NULL-safe. */
static void free_func(vl_func_def *f) {
    if (f == NULL) {
        return;
    }
    vl_free(f->name);
    free_str_list(f->params, f->nparams);
    free_stmt(f->body);
    vl_free(f);
}

/* Free a whole program tree. NULL-safe. */
static void free_program(vl_program *prog) {
    int i;
    if (prog == NULL) {
        return;
    }
    for (i = 0; i < prog->nfuncs; i++) {
        free_func(prog->funcs[i]);
    }
    vl_free(prog->funcs);
    vl_free(prog);
}

/* Advance the cursor: cur takes peek, peek takes the next lexer token. */
static void advance(parser *p) {
    p->cur = p->peek;
    p->peek = vl_lexer_next(&p->lx);
    if (p->peek.kind == TK_ERROR) {
        p_error(p, p->peek.line, "invalid or unterminated token");
    }
}

/* True when the current token is of the given kind. */
static int check(parser *p, vl_tok_kind kind) {
    return p->cur.kind == kind;
}

/* If the current token matches, consume it and return 1; else return 0. */
static int accept(parser *p, vl_tok_kind kind) {
    if (p->cur.kind == kind) {
        advance(p);
        return 1;
    }
    return 0;
}

/* Consume the expected token, or record msg and return 0. */
static int expect(parser *p, vl_tok_kind kind, const char *msg) {
    if (p->cur.kind == kind) {
        advance(p);
        return 1;
    }
    p_error(p, p->cur.line, msg);
    return 0;
}

/* Build an EX_BINARY node over l and r, freeing both on OOM. */
static vl_expr *make_binary(parser *p, int op, int line, vl_expr *l,
                            vl_expr *r) {
    vl_expr *b = new_expr(p, EX_BINARY, line);
    if (b == NULL) {
        free_expr(l);
        free_expr(r);
        return NULL;
    }
    b->as.binary.op = op;
    b->as.binary.l = l;
    b->as.binary.r = r;
    return b;
}

/* Parse a double-quoted string literal, copying and unescaping its content. */
static vl_expr *parse_string(parser *p) {
    int line = p->cur.line;
    const char *s = p->cur.start;
    uint32_t n = p->cur.len;
    uint32_t i = 0, o = 0;
    char *buf;
    vl_expr *e = new_expr(p, EX_STR, line);
    if (e == NULL) {
        return NULL;
    }
    buf = vl_malloc(n ? n : 1);
    if (buf == NULL) {
        p_oom(p);
        free_expr(e);
        return NULL;
    }
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            i++;
            if (i >= n) {
                vl_free(buf);
                p_error(p, line, "unterminated escape in string");
                free_expr(e);
                return NULL;
            }
            switch (s[i]) {
            case 'n': buf[o++] = '\n'; break;
            case 't': buf[o++] = '\t'; break;
            case '\\': buf[o++] = '\\'; break;
            case '"': buf[o++] = '"'; break;
            default:
                vl_free(buf);
                p_error(p, line, "invalid escape in string");
                free_expr(e);
                return NULL;
            }
            i++;
        } else {
            buf[o++] = c;
            i++;
        }
    }
    e->as.str.data = buf;
    e->as.str.len = o;
    advance(p);
    return e;
}

/* Parse an array literal `[ e, e, ... ]` into an EX_ARRAY node. */
static vl_expr *parse_array(parser *p) {
    int line = p->cur.line;
    vl_expr **items = NULL;
    int n = 0, cap = 0;
    vl_expr *e;
    advance(p); /* consume '[' */
    if (!check(p, TK_RBRACKET)) {
        for (;;) {
            void *na;
            vl_expr *it = parse_expr(p);
            if (it == NULL) {
                free_expr_list(items, n);
                return NULL;
            }
            na = ptr_vec_grow(items, n, &cap, sizeof(vl_expr *));
            if (na == NULL) {
                free_expr(it);
                free_expr_list(items, n);
                p_oom(p);
                return NULL;
            }
            items = na;
            items[n++] = it;
            if (accept(p, TK_COMMA)) {
                continue;
            }
            break;
        }
    }
    if (!expect(p, TK_RBRACKET, "expected ']' in array literal")) {
        free_expr_list(items, n);
        return NULL;
    }
    e = new_expr(p, EX_ARRAY, line);
    if (e == NULL) {
        free_expr_list(items, n);
        return NULL;
    }
    e->as.array.items = items;
    e->as.array.n = n;
    return e;
}

/* Parse a map literal `{ k: v, k: v, ... }` into an EX_MAP node. */
static vl_expr *parse_map(parser *p) {
    int line = p->cur.line;
    vl_expr **keys = NULL, **vals = NULL;
    int n = 0, ck = 0, cv = 0;
    vl_expr *e;
    advance(p); /* consume '{' */
    if (!check(p, TK_RBRACE)) {
        for (;;) {
            void *nk, *nv;
            vl_expr *k = parse_expr(p);
            if (k == NULL) {
                free_expr_list(keys, n);
                free_expr_list(vals, n);
                return NULL;
            }
            if (!expect(p, TK_COLON, "expected ':' in map literal")) {
                free_expr(k);
                free_expr_list(keys, n);
                free_expr_list(vals, n);
                return NULL;
            }
            {
                vl_expr *v = parse_expr(p);
                if (v == NULL) {
                    free_expr(k);
                    free_expr_list(keys, n);
                    free_expr_list(vals, n);
                    return NULL;
                }
                nk = ptr_vec_grow(keys, n, &ck, sizeof(vl_expr *));
                if (nk == NULL) {
                    free_expr(k);
                    free_expr(v);
                    free_expr_list(keys, n);
                    free_expr_list(vals, n);
                    p_oom(p);
                    return NULL;
                }
                keys = nk;
                nv = ptr_vec_grow(vals, n, &cv, sizeof(vl_expr *));
                if (nv == NULL) {
                    free_expr(k);
                    free_expr(v);
                    free_expr_list(keys, n);
                    free_expr_list(vals, n);
                    p_oom(p);
                    return NULL;
                }
                vals = nv;
                keys[n] = k;
                vals[n] = v;
                n++;
            }
            if (accept(p, TK_COMMA)) {
                continue;
            }
            break;
        }
    }
    if (!expect(p, TK_RBRACE, "expected '}' in map literal")) {
        free_expr_list(keys, n);
        free_expr_list(vals, n);
        return NULL;
    }
    e = new_expr(p, EX_MAP, line);
    if (e == NULL) {
        free_expr_list(keys, n);
        free_expr_list(vals, n);
        return NULL;
    }
    e->as.map.keys = keys;
    e->as.map.vals = vals;
    e->as.map.n = n;
    return e;
}

/* Parse a primary: literal, identifier, grouping, or array/map literal. */
static vl_expr *parse_primary(parser *p) {
    int line = p->cur.line;
    vl_expr *e;
    switch (p->cur.kind) {
    case TK_INT:
        e = new_expr(p, EX_INT, line);
        if (e == NULL) {
            return NULL;
        }
        e->as.i = p->cur.ival;
        advance(p);
        return e;
    case TK_REAL:
        e = new_expr(p, EX_REAL, line);
        if (e == NULL) {
            return NULL;
        }
        e->as.d = p->cur.rval;
        advance(p);
        return e;
    case TK_STR:
        return parse_string(p);
    case TK_TRUE:
    case TK_FALSE:
        e = new_expr(p, EX_BOOL, line);
        if (e == NULL) {
            return NULL;
        }
        e->as.b = (p->cur.kind == TK_TRUE) ? 1 : 0;
        advance(p);
        return e;
    case TK_NIL:
        e = new_expr(p, EX_NIL, line);
        if (e == NULL) {
            return NULL;
        }
        advance(p);
        return e;
    case TK_IDENT:
        e = new_expr(p, EX_IDENT, line);
        if (e == NULL) {
            return NULL;
        }
        e->as.ident.name = dup_str(p, p->cur.start, p->cur.len);
        if (e->as.ident.name == NULL) {
            free_expr(e);
            return NULL;
        }
        advance(p);
        return e;
    case TK_LPAREN:
        advance(p);
        e = parse_expr(p);
        if (e == NULL) {
            return NULL;
        }
        if (!expect(p, TK_RPAREN, "expected ')' after expression")) {
            free_expr(e);
            return NULL;
        }
        return e;
    case TK_LBRACKET:
        return parse_array(p);
    case TK_LBRACE:
        return parse_map(p);
    default:
        p_error(p, line, "expected expression");
        return NULL;
    }
}

/* Parse a primary followed by any chain of call `(...)` and index `[...]`. */
static vl_expr *parse_postfix(parser *p) {
    vl_expr *e = parse_primary(p);
    if (e == NULL) {
        return NULL;
    }
    for (;;) {
        if (check(p, TK_LPAREN)) {
            int line = p->cur.line;
            vl_expr **args = NULL;
            int n = 0, cap = 0;
            vl_expr *call;
            advance(p); /* consume '(' */
            if (!check(p, TK_RPAREN)) {
                for (;;) {
                    void *na;
                    vl_expr *a = parse_expr(p);
                    if (a == NULL) {
                        free_expr_list(args, n);
                        free_expr(e);
                        return NULL;
                    }
                    na = ptr_vec_grow(args, n, &cap, sizeof(vl_expr *));
                    if (na == NULL) {
                        free_expr(a);
                        free_expr_list(args, n);
                        free_expr(e);
                        p_oom(p);
                        return NULL;
                    }
                    args = na;
                    args[n++] = a;
                    if (accept(p, TK_COMMA)) {
                        continue;
                    }
                    break;
                }
            }
            if (!expect(p, TK_RPAREN, "expected ')' after arguments")) {
                free_expr_list(args, n);
                free_expr(e);
                return NULL;
            }
            call = new_expr(p, EX_CALL, line);
            if (call == NULL) {
                free_expr_list(args, n);
                free_expr(e);
                return NULL;
            }
            call->as.call.callee = e;
            call->as.call.args = args;
            call->as.call.nargs = n;
            e = call;
        } else if (check(p, TK_LBRACKET)) {
            int line = p->cur.line;
            vl_expr *idx;
            vl_expr *ix;
            advance(p); /* consume '[' */
            idx = parse_expr(p);
            if (idx == NULL) {
                free_expr(e);
                return NULL;
            }
            if (!expect(p, TK_RBRACKET, "expected ']' after index")) {
                free_expr(idx);
                free_expr(e);
                return NULL;
            }
            ix = new_expr(p, EX_INDEX, line);
            if (ix == NULL) {
                free_expr(idx);
                free_expr(e);
                return NULL;
            }
            ix->as.index.base = e;
            ix->as.index.index = idx;
            e = ix;
        } else {
            break;
        }
    }
    return e;
}

/* Parse a unary prefix `-`/`!` chain, falling through to postfix. */
static vl_expr *parse_unary(parser *p) {
    if (check(p, TK_MINUS) || check(p, TK_BANG)) {
        int op = (p->cur.kind == TK_MINUS) ? OPK_NEG : OPK_NOT;
        int line = p->cur.line;
        vl_expr *operand;
        vl_expr *u;
        advance(p);
        operand = parse_unary(p);
        if (operand == NULL) {
            return NULL;
        }
        u = new_expr(p, EX_UNARY, line);
        if (u == NULL) {
            free_expr(operand);
            return NULL;
        }
        u->as.unary.op = op;
        u->as.unary.e = operand;
        return u;
    }
    return parse_postfix(p);
}

/* Parse `* / %` multiplicative expressions (left-associative). */
static vl_expr *parse_mul(parser *p) {
    vl_expr *l = parse_unary(p);
    if (l == NULL) {
        return NULL;
    }
    while (check(p, TK_STAR) || check(p, TK_SLASH) || check(p, TK_PERCENT)) {
        int op = (p->cur.kind == TK_STAR)  ? OPK_MUL
                 : (p->cur.kind == TK_SLASH) ? OPK_DIV
                                             : OPK_MOD;
        int line = p->cur.line;
        vl_expr *r;
        advance(p);
        r = parse_unary(p);
        if (r == NULL) {
            free_expr(l);
            return NULL;
        }
        l = make_binary(p, op, line, l, r);
        if (l == NULL) {
            return NULL;
        }
    }
    return l;
}

/* Parse `+ -` additive expressions (left-associative). */
static vl_expr *parse_add(parser *p) {
    vl_expr *l = parse_mul(p);
    if (l == NULL) {
        return NULL;
    }
    while (check(p, TK_PLUS) || check(p, TK_MINUS)) {
        int op = (p->cur.kind == TK_PLUS) ? OPK_ADD : OPK_SUB;
        int line = p->cur.line;
        vl_expr *r;
        advance(p);
        r = parse_mul(p);
        if (r == NULL) {
            free_expr(l);
            return NULL;
        }
        l = make_binary(p, op, line, l, r);
        if (l == NULL) {
            return NULL;
        }
    }
    return l;
}

/* Parse `< <= > >=` comparison expressions (left-associative). */
static vl_expr *parse_cmp(parser *p) {
    vl_expr *l = parse_add(p);
    if (l == NULL) {
        return NULL;
    }
    while (check(p, TK_LT) || check(p, TK_LE) || check(p, TK_GT) ||
           check(p, TK_GE)) {
        int op = (p->cur.kind == TK_LT)   ? OPK_LT
                 : (p->cur.kind == TK_LE) ? OPK_LE
                 : (p->cur.kind == TK_GT) ? OPK_GT
                                          : OPK_GE;
        int line = p->cur.line;
        vl_expr *r;
        advance(p);
        r = parse_add(p);
        if (r == NULL) {
            free_expr(l);
            return NULL;
        }
        l = make_binary(p, op, line, l, r);
        if (l == NULL) {
            return NULL;
        }
    }
    return l;
}

/* Parse `== !=` equality expressions (left-associative). */
static vl_expr *parse_eq(parser *p) {
    vl_expr *l = parse_cmp(p);
    if (l == NULL) {
        return NULL;
    }
    while (check(p, TK_EQ) || check(p, TK_NE)) {
        int op = (p->cur.kind == TK_EQ) ? OPK_EQ : OPK_NE;
        int line = p->cur.line;
        vl_expr *r;
        advance(p);
        r = parse_cmp(p);
        if (r == NULL) {
            free_expr(l);
            return NULL;
        }
        l = make_binary(p, op, line, l, r);
        if (l == NULL) {
            return NULL;
        }
    }
    return l;
}

/* Parse `and` expressions (left-associative). */
static vl_expr *parse_and(parser *p) {
    vl_expr *l = parse_eq(p);
    if (l == NULL) {
        return NULL;
    }
    while (check(p, TK_AND)) {
        int line = p->cur.line;
        vl_expr *r;
        advance(p);
        r = parse_eq(p);
        if (r == NULL) {
            free_expr(l);
            return NULL;
        }
        l = make_binary(p, OPK_AND, line, l, r);
        if (l == NULL) {
            return NULL;
        }
    }
    return l;
}

/* Parse `or` expressions (left-associative); the lowest expression level. */
static vl_expr *parse_or(parser *p) {
    vl_expr *l = parse_and(p);
    if (l == NULL) {
        return NULL;
    }
    while (check(p, TK_OR)) {
        int line = p->cur.line;
        vl_expr *r;
        advance(p);
        r = parse_and(p);
        if (r == NULL) {
            free_expr(l);
            return NULL;
        }
        l = make_binary(p, OPK_OR, line, l, r);
        if (l == NULL) {
            return NULL;
        }
    }
    return l;
}

/* Parse a full expression (entry point for the precedence cascade). */
static vl_expr *parse_expr(parser *p) {
    return parse_or(p);
}

/* Parse `let IDENT = expr ;` into an ST_LET node. */
static vl_stmt *parse_let(parser *p) {
    int line = p->cur.line;
    char *name;
    vl_expr *init;
    vl_stmt *s;
    advance(p); /* consume 'let' */
    if (!check(p, TK_IDENT)) {
        p_error(p, p->cur.line, "expected name after 'let'");
        return NULL;
    }
    name = dup_str(p, p->cur.start, p->cur.len);
    if (name == NULL) {
        return NULL;
    }
    advance(p);
    if (!expect(p, TK_ASSIGN, "expected '=' in let")) {
        vl_free(name);
        return NULL;
    }
    init = parse_expr(p);
    if (init == NULL) {
        vl_free(name);
        return NULL;
    }
    if (!expect(p, TK_SEMI, "expected ';' after let")) {
        vl_free(name);
        free_expr(init);
        return NULL;
    }
    s = new_stmt(p, ST_LET, line);
    if (s == NULL) {
        vl_free(name);
        free_expr(init);
        return NULL;
    }
    s->as.let.name = name;
    s->as.let.init = init;
    return s;
}

/* Parse `if ( expr ) block [ else block ]` into an ST_IF node. */
static vl_stmt *parse_if(parser *p) {
    int line = p->cur.line;
    vl_expr *cond;
    vl_stmt *then_s;
    vl_stmt *else_s = NULL;
    vl_stmt *s;
    advance(p); /* consume 'if' */
    if (!expect(p, TK_LPAREN, "expected '(' after 'if'")) {
        return NULL;
    }
    cond = parse_expr(p);
    if (cond == NULL) {
        return NULL;
    }
    if (!expect(p, TK_RPAREN, "expected ')' after condition")) {
        free_expr(cond);
        return NULL;
    }
    then_s = parse_block(p);
    if (then_s == NULL) {
        free_expr(cond);
        return NULL;
    }
    if (accept(p, TK_ELSE)) {
        else_s = parse_block(p);
        if (else_s == NULL) {
            free_expr(cond);
            free_stmt(then_s);
            return NULL;
        }
    }
    s = new_stmt(p, ST_IF, line);
    if (s == NULL) {
        free_expr(cond);
        free_stmt(then_s);
        free_stmt(else_s);
        return NULL;
    }
    s->as.if_s.cond = cond;
    s->as.if_s.then_s = then_s;
    s->as.if_s.else_s = else_s;
    return s;
}

/* Parse `while ( expr ) block` into an ST_WHILE node. */
static vl_stmt *parse_while(parser *p) {
    int line = p->cur.line;
    vl_expr *cond;
    vl_stmt *body;
    vl_stmt *s;
    advance(p); /* consume 'while' */
    if (!expect(p, TK_LPAREN, "expected '(' after 'while'")) {
        return NULL;
    }
    cond = parse_expr(p);
    if (cond == NULL) {
        return NULL;
    }
    if (!expect(p, TK_RPAREN, "expected ')' after condition")) {
        free_expr(cond);
        return NULL;
    }
    body = parse_block(p);
    if (body == NULL) {
        free_expr(cond);
        return NULL;
    }
    s = new_stmt(p, ST_WHILE, line);
    if (s == NULL) {
        free_expr(cond);
        free_stmt(body);
        return NULL;
    }
    s->as.while_s.cond = cond;
    s->as.while_s.body = body;
    return s;
}

/* Parse `return [expr] ;` into an ST_RETURN node (value may be NULL). */
static vl_stmt *parse_return(parser *p) {
    int line = p->cur.line;
    vl_expr *val = NULL;
    vl_stmt *s;
    advance(p); /* consume 'return' */
    if (!check(p, TK_SEMI)) {
        val = parse_expr(p);
        if (val == NULL) {
            return NULL;
        }
    }
    if (!expect(p, TK_SEMI, "expected ';' after return")) {
        free_expr(val);
        return NULL;
    }
    s = new_stmt(p, ST_RETURN, line);
    if (s == NULL) {
        free_expr(val);
        return NULL;
    }
    s->as.ret.value = val;
    return s;
}

/* Parse `print expr ;` into an ST_PRINT node. */
static vl_stmt *parse_print(parser *p) {
    int line = p->cur.line;
    vl_expr *val;
    vl_stmt *s;
    advance(p); /* consume 'print' */
    val = parse_expr(p);
    if (val == NULL) {
        return NULL;
    }
    if (!expect(p, TK_SEMI, "expected ';' after print")) {
        free_expr(val);
        return NULL;
    }
    s = new_stmt(p, ST_PRINT, line);
    if (s == NULL) {
        free_expr(val);
        return NULL;
    }
    s->as.print.value = val;
    return s;
}

/*
 * Parse an expression statement: `expr ;`, or `target = value ;` when a '='
 * follows and target is an assignable EX_IDENT or EX_INDEX.
 */
static vl_stmt *parse_expr_stmt(parser *p) {
    int line = p->cur.line;
    vl_expr *e = parse_expr(p);
    vl_stmt *s;
    if (e == NULL) {
        return NULL;
    }
    if (check(p, TK_ASSIGN)) {
        int aline = p->cur.line;
        vl_expr *val;
        if (e->kind != EX_IDENT && e->kind != EX_INDEX) {
            p_error(p, aline, "invalid assignment target");
            free_expr(e);
            return NULL;
        }
        advance(p); /* consume '=' */
        val = parse_expr(p);
        if (val == NULL) {
            free_expr(e);
            return NULL;
        }
        if (!expect(p, TK_SEMI, "expected ';' after assignment")) {
            free_expr(e);
            free_expr(val);
            return NULL;
        }
        s = new_stmt(p, ST_ASSIGN, line);
        if (s == NULL) {
            free_expr(e);
            free_expr(val);
            return NULL;
        }
        s->as.assign.target = e;
        s->as.assign.value = val;
        return s;
    }
    if (!expect(p, TK_SEMI, "expected ';' after expression")) {
        free_expr(e);
        return NULL;
    }
    s = new_stmt(p, ST_EXPR, line);
    if (s == NULL) {
        free_expr(e);
        return NULL;
    }
    s->as.expr.e = e;
    return s;
}

/* Dispatch on the leading token to parse a single statement. */
static vl_stmt *parse_stmt(parser *p) {
    switch (p->cur.kind) {
    case TK_LET:
        return parse_let(p);
    case TK_IF:
        return parse_if(p);
    case TK_WHILE:
        return parse_while(p);
    case TK_RETURN:
        return parse_return(p);
    case TK_PRINT:
        return parse_print(p);
    default:
        return parse_expr_stmt(p);
    }
}

/* Parse `{ stmt* }` into an ST_BLOCK node. */
static vl_stmt *parse_block(parser *p) {
    int line = p->cur.line;
    vl_stmt **items = NULL;
    int n = 0, cap = 0;
    vl_stmt *blk;
    if (!expect(p, TK_LBRACE, "expected '{'")) {
        return NULL;
    }
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        void *na;
        vl_stmt *s = parse_stmt(p);
        if (s == NULL) {
            free_stmt_list(items, n);
            return NULL;
        }
        na = ptr_vec_grow(items, n, &cap, sizeof(vl_stmt *));
        if (na == NULL) {
            free_stmt(s);
            free_stmt_list(items, n);
            p_oom(p);
            return NULL;
        }
        items = na;
        items[n++] = s;
    }
    if (!expect(p, TK_RBRACE, "expected '}'")) {
        free_stmt_list(items, n);
        return NULL;
    }
    blk = new_stmt(p, ST_BLOCK, line);
    if (blk == NULL) {
        free_stmt_list(items, n);
        return NULL;
    }
    blk->as.block.items = items;
    blk->as.block.n = n;
    return blk;
}

/* Parse `fn IDENT ( params ) block` into a function definition. */
static vl_func_def *parse_func(parser *p) {
    char *name;
    vl_func_def *f;
    char **params = NULL;
    int np = 0, cap = 0;
    vl_stmt *body;
    advance(p); /* consume 'fn' (the caller verified it) */
    if (!check(p, TK_IDENT)) {
        p_error(p, p->cur.line, "expected function name");
        return NULL;
    }
    name = dup_str(p, p->cur.start, p->cur.len);
    if (name == NULL) {
        return NULL;
    }
    advance(p);
    f = vl_calloc(1, sizeof(*f));
    if (f == NULL) {
        p_oom(p);
        vl_free(name);
        return NULL;
    }
    f->name = name;
    if (!expect(p, TK_LPAREN, "expected '(' after function name")) {
        free_func(f);
        return NULL;
    }
    if (!check(p, TK_RPAREN)) {
        for (;;) {
            void *na;
            char *pn;
            if (!check(p, TK_IDENT)) {
                p_error(p, p->cur.line, "expected parameter name");
                free_str_list(params, np);
                free_func(f);
                return NULL;
            }
            pn = dup_str(p, p->cur.start, p->cur.len);
            if (pn == NULL) {
                free_str_list(params, np);
                free_func(f);
                return NULL;
            }
            advance(p);
            na = ptr_vec_grow(params, np, &cap, sizeof(char *));
            if (na == NULL) {
                vl_free(pn);
                free_str_list(params, np);
                free_func(f);
                p_oom(p);
                return NULL;
            }
            params = na;
            params[np++] = pn;
            if (accept(p, TK_COMMA)) {
                continue;
            }
            break;
        }
    }
    /* Attach params so free_func reclaims them if anything below fails. */
    f->params = params;
    f->nparams = np;
    if (!expect(p, TK_RPAREN, "expected ')' after parameters")) {
        free_func(f);
        return NULL;
    }
    body = parse_block(p);
    if (body == NULL) {
        free_func(f);
        return NULL;
    }
    f->body = body;
    return f;
}

/* Parse the whole program: a sequence of function definitions up to EOF. */
static vl_program *parse_program(parser *p) {
    vl_program *prog = vl_calloc(1, sizeof(*prog));
    vl_func_def **funcs = NULL;
    int n = 0, cap = 0, i;
    if (prog == NULL) {
        p_oom(p);
        return NULL;
    }
    while (!check(p, TK_EOF)) {
        void *na;
        vl_func_def *f;
        if (!check(p, TK_FN)) {
            p_error(p, p->cur.line, "expected 'fn'");
            goto fail;
        }
        f = parse_func(p);
        if (f == NULL) {
            goto fail;
        }
        na = ptr_vec_grow(funcs, n, &cap, sizeof(vl_func_def *));
        if (na == NULL) {
            free_func(f);
            p_oom(p);
            goto fail;
        }
        funcs = na;
        funcs[n++] = f;
    }
    prog->funcs = funcs;
    prog->nfuncs = n;
    return prog;

fail:
    for (i = 0; i < n; i++) {
        free_func(funcs[i]);
    }
    vl_free(funcs);
    vl_free(prog);
    return NULL;
}

/*
 * Parse src[0,len) into a program. On success stores the tree in *out and
 * returns VL_OK; on error returns a non-OK status, writes "line N: <reason>"
 * into err (when provided), and leaves *out untouched.
 */
vl_status vl_parse(const char *src, size_t len, vl_program **out, char *err,
                   size_t errcap) {
    parser p;
    vl_program *prog;

    if (out == NULL || (src == NULL && len != 0)) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "line 0: invalid arguments");
        }
        return VL_ERR_BAD_MODULE;
    }

    p.err = err;
    p.errcap = errcap;
    p.had_error = 0;
    p.status = VL_OK;
    vl_lexer_init(&p.lx, src, len);
    p.cur = vl_lexer_next(&p.lx);
    p.peek = vl_lexer_next(&p.lx);
    if (p.cur.kind == TK_ERROR) {
        p_error(&p, p.cur.line, "invalid or unterminated token");
    } else if (p.peek.kind == TK_ERROR) {
        p_error(&p, p.peek.line, "invalid or unterminated token");
    }

    prog = parse_program(&p);
    if (p.had_error || prog == NULL) {
        free_program(prog);
        if (!p.had_error) {
            p_error(&p, 0, "parse error");
        }
        return p.status;
    }

    *out = prog;
    return VL_OK;
}
