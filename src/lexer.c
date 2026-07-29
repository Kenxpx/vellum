/*
 * lexer.c - the tokenizer for the Vellum source language.
 *
 * Turns raw source bytes into a stream of vl_token values for the parser.
 * Whitespace and '#' line comments are skipped; numbers, double-quoted strings,
 * identifiers, keywords, and operators/punctuation are recognized. The lexer
 * never allocates and never reads past src+len: string and identifier tokens
 * point back into the caller's source buffer with an explicit length, and the
 * raw (still-escaped) bytes of a string are handed to the parser to unescape.
 */
#include "lexer.h"

#include <stdlib.h>

#include "internal.h"

/* True for an ASCII decimal digit. */
static int is_digit(int c) {
    return c >= '0' && c <= '9';
}

/* True for a character that may begin an identifier. */
static int is_ident_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

/* True for a character that may continue an identifier. */
static int is_ident_part(int c) {
    return is_ident_start(c) || is_digit(c);
}

/* Build a token of the given kind spanning src[start_pos .. lx->pos). */
static vl_token make_token(const vl_lexer *lx, vl_tok_kind kind,
                           size_t start_pos, int line) {
    vl_token t;
    t.kind = kind;
    t.start = lx->src + start_pos;
    t.len = (uint32_t)(lx->pos - start_pos);
    t.line = line;
    t.ival = 0;
    t.rval = 0.0;
    return t;
}

/*
 * Classify the identifier spanning [s, s+n) as a keyword token kind, or as
 * TK_IDENT when it matches none of the reserved words.
 */
static vl_tok_kind keyword_kind(const char *s, size_t n) {
    switch (n) {
    case 2:
        if (memcmp(s, "fn", 2) == 0) return TK_FN;
        if (memcmp(s, "if", 2) == 0) return TK_IF;
        if (memcmp(s, "or", 2) == 0) return TK_OR;
        break;
    case 3:
        if (memcmp(s, "let", 3) == 0) return TK_LET;
        if (memcmp(s, "nil", 3) == 0) return TK_NIL;
        if (memcmp(s, "and", 3) == 0) return TK_AND;
        break;
    case 4:
        if (memcmp(s, "else", 4) == 0) return TK_ELSE;
        if (memcmp(s, "true", 4) == 0) return TK_TRUE;
        break;
    case 5:
        if (memcmp(s, "while", 5) == 0) return TK_WHILE;
        if (memcmp(s, "print", 5) == 0) return TK_PRINT;
        if (memcmp(s, "false", 5) == 0) return TK_FALSE;
        break;
    case 6:
        if (memcmp(s, "return", 6) == 0) return TK_RETURN;
        break;
    default:
        break;
    }
    return TK_IDENT;
}

/*
 * Scan a numeric literal beginning at the current position (which must be a
 * digit). Produces TK_INT, or TK_REAL when a fractional part or exponent is
 * present, with the decoded value stored in the token.
 */
static vl_token scan_number(vl_lexer *lx, size_t start_pos, int line) {
    int is_real = 0;
    vl_token t;

    while (lx->pos < lx->len && is_digit((unsigned char)lx->src[lx->pos])) {
        lx->pos++;
    }
    if (lx->pos < lx->len && lx->src[lx->pos] == '.') {
        is_real = 1;
        lx->pos++;
        while (lx->pos < lx->len && is_digit((unsigned char)lx->src[lx->pos])) {
            lx->pos++;
        }
    }
    if (lx->pos < lx->len &&
        (lx->src[lx->pos] == 'e' || lx->src[lx->pos] == 'E')) {
        size_t save = lx->pos;
        size_t p = lx->pos + 1;
        if (p < lx->len && (lx->src[p] == '+' || lx->src[p] == '-')) {
            p++;
        }
        if (p < lx->len && is_digit((unsigned char)lx->src[p])) {
            is_real = 1;
            p++;
            while (p < lx->len && is_digit((unsigned char)lx->src[p])) {
                p++;
            }
            lx->pos = p;
        } else {
            /* Bare 'e' with no exponent digits: not part of the number. */
            lx->pos = save;
        }
    }

    t = make_token(lx, is_real ? TK_REAL : TK_INT, start_pos, line);

    /*
     * strtoll/strtod require a NUL-terminated string, but the source buffer
     * may not be terminated at the token's end. Copy the span into a bounded
     * stack buffer first so parsing never reads past src+len.
     */
    {
        char buf[64];
        size_t n = t.len < sizeof(buf) - 1 ? t.len : sizeof(buf) - 1;
        memcpy(buf, t.start, n);
        buf[n] = '\0';
        if (is_real) {
            t.rval = strtod(buf, NULL);
        } else {
            t.ival = (int64_t)strtoll(buf, NULL, 10);
        }
    }
    return t;
}

/* Initialize a lexer over [src, src+len); the first line is line 1. */
void vl_lexer_init(vl_lexer *lx, const char *src, size_t len) {
    lx->src = src;
    lx->len = len;
    lx->pos = 0;
    lx->line = 1;
}

/*
 * Produce the next token. Whitespace, newlines (which advance the line count),
 * and '#'-to-end-of-line comments are skipped first. At end of input TK_EOF is
 * returned repeatedly; an unrecognized byte yields TK_ERROR.
 */
vl_token vl_lexer_next(vl_lexer *lx) {
    size_t start_pos;
    int line;
    char c;

    /* Skip whitespace and line comments. */
    for (;;) {
        if (lx->pos >= lx->len) {
            return make_token(lx, TK_EOF, lx->pos, lx->line);
        }
        c = lx->src[lx->pos];
        if (c == '\n') {
            lx->line++;
            lx->pos++;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' ||
                   c == '\v') {
            lx->pos++;
        } else if (c == '#') {
            lx->pos++;
            while (lx->pos < lx->len && lx->src[lx->pos] != '\n') {
                lx->pos++;
            }
        } else {
            break;
        }
    }

    start_pos = lx->pos;
    line = lx->line;
    c = lx->src[lx->pos];

    if (is_digit((unsigned char)c)) {
        return scan_number(lx, start_pos, line);
    }

    if (is_ident_start((unsigned char)c)) {
        vl_tok_kind kind;
        lx->pos++;
        while (lx->pos < lx->len &&
               is_ident_part((unsigned char)lx->src[lx->pos])) {
            lx->pos++;
        }
        kind = keyword_kind(lx->src + start_pos, lx->pos - start_pos);
        return make_token(lx, kind, start_pos, line);
    }

    if (c == '"') {
        vl_token t;
        size_t inner_start;
        lx->pos++; /* consume opening quote */
        inner_start = lx->pos;
        while (lx->pos < lx->len && lx->src[lx->pos] != '"') {
            if (lx->src[lx->pos] == '\\') {
                /* Skip the escape and its following byte, if present. */
                lx->pos++;
                if (lx->pos < lx->len) {
                    lx->pos++;
                }
            } else {
                lx->pos++;
            }
        }
        if (lx->pos >= lx->len) {
            /* Unterminated string literal. */
            return make_token(lx, TK_ERROR, start_pos, line);
        }
        t.kind = TK_STR;
        t.start = lx->src + inner_start;
        t.len = (uint32_t)(lx->pos - inner_start);
        t.line = line;
        t.ival = 0;
        t.rval = 0.0;
        lx->pos++; /* consume closing quote */
        return t;
    }

    /* Operators and punctuation. */
    lx->pos++;
    switch (c) {
    case '(': return make_token(lx, TK_LPAREN, start_pos, line);
    case ')': return make_token(lx, TK_RPAREN, start_pos, line);
    case '{': return make_token(lx, TK_LBRACE, start_pos, line);
    case '}': return make_token(lx, TK_RBRACE, start_pos, line);
    case '[': return make_token(lx, TK_LBRACKET, start_pos, line);
    case ']': return make_token(lx, TK_RBRACKET, start_pos, line);
    case ',': return make_token(lx, TK_COMMA, start_pos, line);
    case ';': return make_token(lx, TK_SEMI, start_pos, line);
    case ':': return make_token(lx, TK_COLON, start_pos, line);
    case '+': return make_token(lx, TK_PLUS, start_pos, line);
    case '-': return make_token(lx, TK_MINUS, start_pos, line);
    case '*': return make_token(lx, TK_STAR, start_pos, line);
    case '/': return make_token(lx, TK_SLASH, start_pos, line);
    case '%': return make_token(lx, TK_PERCENT, start_pos, line);
    case '=':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++;
            return make_token(lx, TK_EQ, start_pos, line);
        }
        return make_token(lx, TK_ASSIGN, start_pos, line);
    case '!':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++;
            return make_token(lx, TK_NE, start_pos, line);
        }
        return make_token(lx, TK_BANG, start_pos, line);
    case '<':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++;
            return make_token(lx, TK_LE, start_pos, line);
        }
        return make_token(lx, TK_LT, start_pos, line);
    case '>':
        if (lx->pos < lx->len && lx->src[lx->pos] == '=') {
            lx->pos++;
            return make_token(lx, TK_GE, start_pos, line);
        }
        return make_token(lx, TK_GT, start_pos, line);
    default:
        break;
    }

    return make_token(lx, TK_ERROR, start_pos, line);
}
