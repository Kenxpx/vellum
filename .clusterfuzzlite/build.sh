#!/bin/bash -eu
#
# ClusterFuzzLite build entry point for the Vellum VM.
#
# Compiles the library and every fuzz harness into $OUT using the toolchain and
# flags supplied by the fuzzing environment ($CC, $CXX, $CFLAGS, $CXXFLAGS,
# $LIB_FUZZING_ENGINE). Fully offline: it only compiles sources already present
# in the repository. The working directory is the repository root.

work="$(mktemp -d)"

# --- library ---------------------------------------------------------------
libobjs=""
for f in src/*.c; do
    obj="$work/$(basename "${f%.c}").o"
    $CC $CFLAGS -Iinclude -Isrc -c "$f" -o "$obj"
    libobjs="$libobjs $obj"
done
ar rcs "$work/libvellum.a" $libobjs

# --- fuzz targets ----------------------------------------------------------
# Each fuzz/<name>_fuzzer.c is a distinct entry point. standalone_main.c is a
# local driver (defines main) and is intentionally excluded.
for h in fuzz/*_fuzzer.c; do
    name="$(basename "${h%.c}")"
    $CC $CFLAGS -Iinclude -Isrc -c "$h" -o "$work/$name.o"
    $CXX $CXXFLAGS "$work/$name.o" "$work/libvellum.a" $LIB_FUZZING_ENGINE \
        -o "$OUT/$name"
done

# --- seed corpora ----------------------------------------------------------
for h in fuzz/*_fuzzer.c; do
    name="$(basename "${h%.c}")"
    if [ -d "fuzz/corpus/$name" ]; then
        (cd "fuzz/corpus/$name" && zip -q -r "$OUT/${name}_seed_corpus.zip" .)
    fi
done

# --- dictionary ------------------------------------------------------------
if [ -f fuzz/dictionary.txt ]; then
    for h in fuzz/*_fuzzer.c; do
        name="$(basename "${h%.c}")"
        cp fuzz/dictionary.txt "$OUT/${name}.dict"
    done
fi
