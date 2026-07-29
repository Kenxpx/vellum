# Building Vellum

Vellum is a small stack-based bytecode VM written in portable C11. It has no
third-party dependencies; a C compiler and `make` are all that is required to
build the library, run the tests, and drive the fuzz harnesses locally.

## Prerequisites

- A C11-capable compiler (`gcc` or `clang`). The tree is built with
  `-std=c11 -Wall -Wextra` and is expected to compile without warnings.
- GNU `make`.
- For the ClusterFuzzLite path only: `clang` with libFuzzer and the
  sanitizers (`-fsanitize=fuzzer,address,undefined`). This is not needed for
  ordinary builds or for local fuzzing with the standalone driver.

## Make targets

- `make all` (the default) - builds the static library and any tools. This is
  what runs when you invoke `make` with no arguments.
- `make test` - builds and runs the test binaries under `tests/`, printing a
  pass/fail summary from the harness.
- `make fuzz-local` - builds each fuzz target against `fuzz/standalone_main.c`,
  a libFuzzer-free driver that replays input files named on the command line
  through `LLVMFuzzerTestOneInput`. Use it to reproduce a crashing input
  without a fuzzing engine installed.
- `make clean` - removes all build output (the `build/` directory and stray
  object files).

Build artifacts land in `build/` and are ignored by git.

## Local checks with the page-guard debug allocator

For local memory-safety checks, build with the page-guarded debug allocator
enabled:

```
make GUARD=1
make test GUARD=1
make fuzz-local GUARD=1
```

`GUARD=1` compiles the tree with `-DVL_GUARD_ALLOC`, which routes every
allocation made through `vl_malloc`/`vl_calloc`/`vl_realloc`/`vl_free` to the
debug allocator in `src/dbgalloc.c`. Each allocation is placed so that its last
byte abuts an inaccessible guard page, turning an off-the-end read or write
into an immediate fault instead of silent corruption, and freeing unmaps the
pages so use-after-free is caught too. This is slower and far more memory-hungry
than the release allocator, so use it for reproduction and checking rather than
for production builds. Combining `make fuzz-local GUARD=1` with a saved
crashing input is the recommended way to localize a fault.

## Fuzzing in CI

The fuzz targets are built for continuous fuzzing by ClusterFuzzLite, which
invokes `.clusterfuzzlite/build.sh`. That script compiles the same targets with
`clang` and libFuzzer plus the address and undefined-behavior sanitizers, rather
than the standalone driver used by `make fuzz-local`. There is no need to run it
by hand; the CI infrastructure calls it automatically.
