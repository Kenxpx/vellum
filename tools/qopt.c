/*
 * qopt.c - command-line peephole optimizer for Vellum bytecode modules.
 *
 * Usage: qopt <in.qbc> [out.qbc]
 * Reads the module file into memory, loads and structurally validates it,
 * verifies its bytecode, then runs the size-preserving peephole optimizer and
 * reports how many instructions were neutralized to NOPs. This tool does not
 * re-serialize the module: the optional output path is accepted for interface
 * compatibility but the loaded module is not written back. Any failure is
 * reported to stderr and the process exits with status 1.
 */
#include <stdio.h>
#include <stdlib.h>

#include "vellum/alloc.h"
#include "vellum/error.h"
#include "module.h"
#include "optimize.h"
#include "verify.h"

/* Read the entire file at path into a freshly allocated buffer. */
static vl_status read_file(const char *path, uint8_t **out_data, size_t *out_size)
{
    FILE *f;
    long len;
    size_t size, got;
    uint8_t *buf;

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
    *out_size = size;
    return VL_OK;
}

/* Entry point: load, verify, and optimize a module, then report the count. */
int main(int argc, char **argv)
{
    vl_status st;
    uint8_t *data;
    size_t size;
    vl_module *m;
    uint32_t removed;

    if (argc < 2) {
        fprintf(stderr, "usage: qopt <in.qbc> [out.qbc]\n");
        return 1;
    }

    st = read_file(argv[1], &data, &size);
    if (st != VL_OK) {
        fprintf(stderr, "qopt: %s\n", vl_status_str(st));
        return 1;
    }

    st = vl_module_load(data, size, &m);
    vl_free(data);
    if (st != VL_OK) {
        fprintf(stderr, "qopt: %s\n", vl_status_str(st));
        return 1;
    }

    st = vl_verify_module(m);
    if (st != VL_OK) {
        fprintf(stderr, "qopt: %s\n", vl_status_str(st));
        vl_module_free(m);
        return 1;
    }

    removed = 0;
    st = vl_optimize_module(m, &removed);
    if (st != VL_OK) {
        fprintf(stderr, "qopt: %s\n", vl_status_str(st));
        vl_module_free(m);
        return 1;
    }

    fprintf(stderr, "neutralized %u instructions\n", removed);

    vl_module_free(m);
    return 0;
}
