/*
 * qrun.c - command-line runner for Vellum bytecode modules.
 *
 * Usage: qrun <file.qbc>
 * Reads the module file into memory, loads and structurally validates it,
 * verifies its bytecode, runs the entry function, and prints the resulting
 * value to stdout as "=> <value>". Any failure is reported to stderr and the
 * process exits with status 1.
 */
#include <stdio.h>
#include <stdlib.h>

#include "vellum/alloc.h"
#include "vellum/error.h"
#include "dump.h"
#include "module.h"
#include "obj.h"
#include "verify.h"
#include "vm.h"

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

/* Entry point: load, verify, run a module, and print its result. */
int main(int argc, char **argv)
{
    vl_status st;
    uint8_t *data;
    size_t size;
    vl_module *m;
    vl_value result;

    if (argc < 2) {
        fprintf(stderr, "usage: qrun <file.qbc>\n");
        return 1;
    }

    st = read_file(argv[1], &data, &size);
    if (st != VL_OK) {
        fprintf(stderr, "qrun: %s\n", vl_status_str(st));
        return 1;
    }

    st = vl_module_load(data, size, &m);
    vl_free(data);
    if (st != VL_OK) {
        fprintf(stderr, "qrun: %s\n", vl_status_str(st));
        return 1;
    }

    st = vl_verify_module(m);
    if (st != VL_OK) {
        fprintf(stderr, "qrun: %s\n", vl_status_str(st));
        vl_module_free(m);
        return 1;
    }

    result = vl_nil();
    st = vl_run_module(m, NULL, &result);
    if (st != VL_OK) {
        fprintf(stderr, "qrun: %s\n", vl_status_str(st));
        vl_val_release(result);
        vl_module_free(m);
        return 1;
    }

    fputs("=> ", stdout);
    vl_dump_value(stdout, result);
    fputc('\n', stdout);

    vl_val_release(result);
    vl_module_free(m);
    return 0;
}
