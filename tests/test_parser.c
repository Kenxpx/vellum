/*
 * test_parser.c - unit tests for the Vellum recursive-descent parser (parser.h).
 *
 * Parses a small function and asserts the shape of the resulting AST (function
 * count/name/arity, the block body, a let with a precedence-correct binary
 * initializer, and a trailing return), then confirms a malformed program is
 * rejected with a non-OK status and no crash.
 */
#include "parser.h"
#include "harness.h"

/* Parse a well-formed function and verify the AST structure it produces. */
static void test_parse_ok(void)
{
    const char *src = "fn main() { let x = 1 + 2 * 3; return x; }";
    vl_program *prog = NULL;
    char err[128];
    vl_status st = vl_parse(src, strlen(src), &prog, err, sizeof(err));

    CHECK_OK(st);
    CHECK(prog != NULL);
    if (st != VL_OK || prog == NULL) {
        vl_ast_free_program(prog);
        return;
    }

    CHECK_EQ(prog->nfuncs, 1);
    if (prog->nfuncs != 1) {
        vl_ast_free_program(prog);
        return;
    }

    vl_func_def *fn = prog->funcs[0];
    CHECK(fn != NULL);
    CHECK(fn->name != NULL && strcmp(fn->name, "main") == 0);
    CHECK_EQ(fn->nparams, 0);

    vl_stmt *body = fn->body;
    CHECK(body != NULL);
    if (body == NULL) {
        vl_ast_free_program(prog);
        return;
    }
    CHECK_EQ(body->kind, ST_BLOCK);
    CHECK_EQ(body->as.block.n, 2);
    if (body->kind != ST_BLOCK || body->as.block.n != 2) {
        vl_ast_free_program(prog);
        return;
    }

    /* First statement: let x = 1 + (2 * 3). */
    vl_stmt *s0 = body->as.block.items[0];
    CHECK(s0 != NULL);
    CHECK_EQ(s0->kind, ST_LET);
    if (s0 != NULL && s0->kind == ST_LET) {
        CHECK(s0->as.let.name != NULL && strcmp(s0->as.let.name, "x") == 0);

        vl_expr *init = s0->as.let.init;
        CHECK(init != NULL);
        if (init != NULL) {
            CHECK_EQ(init->kind, EX_BINARY);
            CHECK_EQ(init->as.binary.op, OPK_ADD);
            if (init->kind == EX_BINARY && init->as.binary.op == OPK_ADD) {
                /* Precedence: '*' binds tighter, so it is the right child. */
                vl_expr *rhs = init->as.binary.r;
                CHECK(rhs != NULL);
                if (rhs != NULL) {
                    CHECK_EQ(rhs->kind, EX_BINARY);
                    CHECK_EQ(rhs->as.binary.op, OPK_MUL);
                }
            }
        }
    }

    /* Second statement: return x. */
    vl_stmt *s1 = body->as.block.items[1];
    CHECK(s1 != NULL);
    CHECK_EQ(s1->kind, ST_RETURN);

    vl_ast_free_program(prog);
}

/* Malformed source must fail gracefully with a non-OK status. */
static void test_parse_error(void)
{
    const char *src = "fn main() { let = ; }";
    vl_program *prog = NULL;
    char err[128];
    vl_status st = vl_parse(src, strlen(src), &prog, err, sizeof(err));

    CHECK(st != VL_OK);
    /* On failure the tree must not be handed back; free defensively anyway. */
    vl_ast_free_program(prog);
}

/* Run both parser tests and report the aggregate result. */
int main(void)
{
    test_parse_ok();
    test_parse_error();
    return TEST_SUMMARY();
}
