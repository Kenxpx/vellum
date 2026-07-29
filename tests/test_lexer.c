/*
 * test_lexer.c - unit tests for the Vellum lexer (lexer.h).
 *
 * Tokenizes a small program and asserts the token kinds appear in order with
 * the expected integer value, then exercises a string literal, a real number,
 * and a two-character operator.
 */
#include "harness.h"
#include "lexer.h"

/* Tokenize a full statement and verify every token kind, in order. */
static void test_statement(void) {
    const char *src = "fn main() { let x = 12 + 3; }";
    vl_lexer lx;
    vl_lexer_init(&lx, src, strlen(src));

    static const vl_tok_kind expect[] = {
        TK_FN, TK_IDENT, TK_LPAREN, TK_RPAREN, TK_LBRACE,
        TK_LET, TK_IDENT, TK_ASSIGN, TK_INT, TK_PLUS,
        TK_INT, TK_SEMI, TK_RBRACE, TK_EOF
    };
    size_t n = sizeof(expect) / sizeof(expect[0]);

    int first_int_seen = 0;
    for (size_t i = 0; i < n; i++) {
        vl_token t = vl_lexer_next(&lx);
        CHECK_EQ(t.kind, expect[i]);
        if (t.kind == TK_INT && !first_int_seen) {
            first_int_seen = 1;
            CHECK_EQ(t.ival, 12);
        }
    }
    CHECK(first_int_seen);
}

/* A string literal should tokenize as TK_STR. */
static void test_string(void) {
    const char *src = "\"hello\"";
    vl_lexer lx;
    vl_lexer_init(&lx, src, strlen(src));
    vl_token t = vl_lexer_next(&lx);
    CHECK_EQ(t.kind, TK_STR);
}

/* A fractional literal should tokenize as TK_REAL with the right value. */
static void test_real(void) {
    const char *src = "3.5";
    vl_lexer lx;
    vl_lexer_init(&lx, src, strlen(src));
    vl_token t = vl_lexer_next(&lx);
    CHECK_EQ(t.kind, TK_REAL);
    CHECK(t.rval == 3.5);
}

/* A two-character operator ">=" should tokenize as a single TK_GE. */
static void test_two_char_op(void) {
    const char *src = ">=";
    vl_lexer lx;
    vl_lexer_init(&lx, src, strlen(src));
    vl_token t = vl_lexer_next(&lx);
    CHECK_EQ(t.kind, TK_GE);
    CHECK_EQ(vl_lexer_next(&lx).kind, TK_EOF);
}

/* Entry point: run every lexer test and report the summary. */
int main(void) {
    test_statement();
    test_string();
    test_real();
    test_two_char_op();
    return TEST_SUMMARY();
}
