/*
 * qasm.c - command-line assembler for Vellum.
 *
 * Usage: qasm <in.qasm> [out.qbc]
 * Reads the whole assembly source into memory, assembles it into a .qbc
 * module, and writes the bytes to the output path. When no output path is
 * given it is derived from the input by replacing its extension with ".qbc".
 * On assembly failure the message from vl_assemble is printed to stderr.
 */
#include <stdio.h>
#include <stdlib.h>

#include "asm.h"

/* Read the entire text file at path into a freshly allocated buffer. */
static vl_status read_text_file(const char *path, char **out_data, size_t *out_len)
{
    FILE *f;
    long len;
    size_t size, got;
    char *buf;

    f = fopen(path, "rb");
    if (!f) {
        return VL_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return VL_ERR_IO;
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return VL_ERR_IO;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return VL_ERR_IO;
    }
    size = (size_t)len;
    /* Allocate at least one byte so vl_malloc never sees a zero size. */
    buf = vl_malloc(size ? size : 1);
    if (!buf) {
        fclose(f);
        return VL_ERR_OOM;
    }
    got = size ? fread(buf, 1, size, f) : 0;
    if (got != size) {
        vl_free(buf);
        fclose(f);
        return VL_ERR_IO;
    }
    fclose(f);
    *out_data = buf;
    *out_len = size;
    return VL_OK;
}

/*
 * Build an output path from the input by replacing its final extension with
 * ".qbc" (or appending it when there is none). Returns a freshly allocated
 * string, or NULL on allocation failure.
 */
static char *derive_out_path(const char *in)
{
    static const char suffix[] = ".qbc";
    size_t in_len, base_len, i, total;
    char *out;

    in_len = 0;
    while (in[in_len] != '\0') {
        in_len++;
    }
    /* Keep everything up to a '.' in the final path component. */
    base_len = in_len;
    for (i = in_len; i > 0; i--) {
        char c = in[i - 1];
        if (c == '/' || c == '\\') {
            break;
        }
        if (c == '.') {
            base_len = i - 1;
            break;
        }
    }
    if (!vl_size_add(base_len, sizeof(suffix), &total)) {
        return NULL;
    }
    out = vl_malloc(total);
    if (!out) {
        return NULL;
    }
    for (i = 0; i < base_len; i++) {
        out[i] = in[i];
    }
    for (i = 0; i < sizeof(suffix); i++) {
        out[base_len + i] = suffix[i];
    }
    return out;
}

/* Write len bytes from data to the file at path. */
static vl_status write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f;
    size_t put;

    f = fopen(path, "wb");
    if (!f) {
        return VL_ERR_IO;
    }
    put = len ? fwrite(data, 1, len, f) : 0;
    if (put != len) {
        fclose(f);
        return VL_ERR_IO;
    }
    if (fclose(f) != 0) {
        return VL_ERR_IO;
    }
    return VL_OK;
}

/* Entry point: read source, assemble, and write the resulting module. */
int main(int argc, char **argv)
{
    vl_status st;
    char *text;
    size_t text_len;
    uint8_t *out;
    size_t out_len;
    char errbuf[256];
    char *out_path;
    int owns_out_path;

    if (argc < 2) {
        fprintf(stderr, "usage: qasm <in.qasm> [out.qbc]\n");
        return 1;
    }

    st = read_text_file(argv[1], &text, &text_len);
    if (st != VL_OK) {
        fprintf(stderr, "qasm: %s\n", vl_status_str(st));
        return 1;
    }

    errbuf[0] = '\0';
    st = vl_assemble(text, text_len, &out, &out_len, errbuf, sizeof(errbuf));
    vl_free(text);
    if (st != VL_OK) {
        fprintf(stderr, "qasm: %s\n", errbuf[0] ? errbuf : vl_status_str(st));
        return 1;
    }

    if (argc >= 3) {
        out_path = argv[2];
        owns_out_path = 0;
    } else {
        out_path = derive_out_path(argv[1]);
        owns_out_path = 1;
        if (!out_path) {
            vl_free(out);
            fprintf(stderr, "qasm: %s\n", vl_status_str(VL_ERR_OOM));
            return 1;
        }
    }

    st = write_file(out_path, out, out_len);
    if (st != VL_OK) {
        fprintf(stderr, "qasm: %s\n", vl_status_str(st));
        if (owns_out_path) {
            vl_free(out_path);
        }
        vl_free(out);
        return 1;
    }

    printf("%s: %zu bytes\n", out_path, out_len);
    if (owns_out_path) {
        vl_free(out_path);
    }
    vl_free(out);
    return 0;
}
