/*
 * standalone_main.c - a libFuzzer-free driver for the harnesses.
 *
 * Links against one *_fuzzer.c and replays each file named on the command line
 * through LLVMFuzzerTestOneInput. Combined with the page-guard debug allocator
 * (-DRX_GUARD_ALLOC) this reproduces a crashing input locally without a fuzzing
 * engine; the ClusterFuzzLite build uses libFuzzer instead of this file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
    int i;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <input-file>...\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        long len;
        uint8_t *buf;
        size_t got;
        if (!f) {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            return 2;
        }
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len < 0) {
            fclose(f);
            return 2;
        }
        buf = (uint8_t *)malloc((size_t)len ? (size_t)len : 1);
        got = fread(buf, 1, (size_t)len, f);
        fclose(f);
        fprintf(stderr, "[driver] %s (%zu bytes)\n", argv[i], got);
        LLVMFuzzerTestOneInput(buf, got);
        free(buf);
    }
    fprintf(stderr, "[driver] all inputs processed without a crash\n");
    return 0;
}
