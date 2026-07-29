/*
 * test_disasm.c - unit test for the module disassembler (disasm.h).
 *
 * Builds a tiny single-function module with the builder (a PUSHK of an integer
 * constant followed by RET), serializes and loads it, then disassembles it to a
 * temporary file and checks that output was produced and mentions "pushk".
 */
#include "internal.h"
#include "module.h"
#include "opcodes.h"
#include "builder.h"
#include "disasm.h"
#include "harness.h"

/* Build, load, disassemble a small module and verify the output. */
static void test_disasm_output(void)
{
    vl_builder *b;
    uint8_t *buf = NULL;
    size_t len = 0;
    vl_module *m = NULL;
    int kidx, fidx;
    FILE *tmp;
    long pos;
    char text[4096];
    size_t nread;

    b = vl_builder_new();
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }

    kidx = vl_builder_const_int(b, 42);
    CHECK(kidx >= 0);

    fidx = vl_builder_begin_func(b, 0, 0);
    CHECK(fidx >= 0);
    vl_builder_set_entry(b, fidx);

    CHECK_OK(vl_builder_emit_u16(b, fidx, OP_PUSHK, (uint16_t)kidx));
    CHECK_OK(vl_builder_emit(b, fidx, OP_RET));

    CHECK_OK(vl_builder_finish(b, &buf, &len));
    CHECK(buf != NULL);
    vl_builder_free(b);

    if (buf == NULL) {
        return;
    }

    CHECK_OK(vl_module_load(buf, len, &m));
    vl_free(buf);
    CHECK(m != NULL);
    if (m == NULL) {
        return;
    }

    tmp = tmpfile();
    CHECK(tmp != NULL);
    if (tmp == NULL) {
        vl_module_free(m);
        return;
    }

    CHECK_OK(vl_disasm_module(m, tmp));

    pos = ftell(tmp);
    CHECK(pos > 0);

    /* Read the produced text back and confirm it mentions the pushk opcode. */
    rewind(tmp);
    nread = fread(text, 1, sizeof(text) - 1, tmp);
    text[nread] = '\0';
    CHECK(strstr(text, "pushk") != NULL);

    fclose(tmp);
    vl_module_free(m);
}

/* Run the disassembler test. */
int main(void)
{
    test_disasm_output();
    return TEST_SUMMARY();
}
