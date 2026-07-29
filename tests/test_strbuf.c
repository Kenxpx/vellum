/*
 * test_strbuf.c - unit tests for the growable text buffer in strbuf.h.
 *
 * Exercises byte/string/formatted appends, the NUL-terminated view, and
 * repeated appends that force several buffer growths.
 */
#include "harness.h"

#include "strbuf.h"

/* Build "hello" byte-by-byte and via puts, then append formatted text and a
 * long run of strings to force growth; verify contents, length, and NUL. */
int main(void)
{
    vl_strbuf sb;

    vl_strbuf_init(&sb);
    CHECK_EQ(sb.len, 0);

    /* putc/puts build "hello". */
    CHECK_OK(vl_strbuf_putc(&sb, 'h'));
    CHECK_OK(vl_strbuf_putc(&sb, 'e'));
    CHECK_OK(vl_strbuf_puts(&sb, "llo"));
    CHECK(strcmp(vl_strbuf_cstr(&sb), "hello") == 0);
    CHECK_EQ(sb.len, 5);

    /* printf appends " 42!" -> "hello 42!". */
    CHECK_OK(vl_strbuf_printf(&sb, " %d!", 42));
    CHECK(strcmp(vl_strbuf_cstr(&sb), "hello 42!") == 0);

    /* Force several grows: 100 appends of a 10-char string. */
    {
        int i;
        for (i = 0; i < 100; i++) {
            CHECK_OK(vl_strbuf_puts(&sb, "0123456789"));
        }
    }

    /* "hello 42!" is 9 bytes, plus 100 * 10 = 1000 -> 1009. */
    CHECK_EQ(sb.len, 1009);

    /* Buffer must be NUL-terminated at len. */
    {
        const char *s = vl_strbuf_cstr(&sb);
        CHECK_EQ(strlen(s), 1009);
        CHECK_EQ(s[sb.len], '\0');
    }

    vl_strbuf_dispose(&sb);

    return TEST_SUMMARY();
}
