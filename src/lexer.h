/*
 * lexer.h - the tokenizer for the Vellum source language.
 *
 * Splits source text into a stream of tokens for the parser. Numbers, strings,
 * identifiers, keywords, operators, and punctuation are recognized; whitespace
 * and '#' line comments are skipped. The lexer never allocates: string and
 * identifier tokens point back into the source buffer with an explicit length.
 */
#ifndef VELLUM_LEXER_H
#define VELLUM_LEXER_H

#include "internal.h"

typedef enum vl_tok_kind {
    TK_EOF = 0,
    TK_ERROR,
    TK_INT, TK_REAL, TK_STR, TK_IDENT,
    /* keywords */
    TK_FN, TK_LET, TK_IF, TK_ELSE, TK_WHILE, TK_RETURN, TK_PRINT,
    TK_TRUE, TK_FALSE, TK_NIL, TK_AND, TK_OR,
    /* punctuation / operators */
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_SEMI, TK_COLON,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_ASSIGN, TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_BANG
} vl_tok_kind;

typedef struct vl_token {
    vl_tok_kind kind;
    const char *start; /* into the source buffer                 */
    uint32_t len;
    int line;
    int64_t ival;      /* valid for TK_INT                       */
    double rval;       /* valid for TK_REAL                      */
} vl_token;

typedef struct vl_lexer {
    const char *src;
    size_t len;
    size_t pos;
    int line;
} vl_lexer;

void vl_lexer_init(vl_lexer *lx, const char *src, size_t len);

/* Produce the next token. At end of input yields TK_EOF repeatedly. */
vl_token vl_lexer_next(vl_lexer *lx);

#endif /* VELLUM_LEXER_H */
