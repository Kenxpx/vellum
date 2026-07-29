# Vellum

Vellum is a small stack-based bytecode VM written in C11. It loads a compiled
module, verifies its bytecode, and runs it on a stack machine. Programs get a
constant pool, first-class functions, closures with captured upvalues, arrays
and maps, tail calls, and a reference-counted object heap that owns all of the
above.

The public surface is deliberately tiny: `vl_exec(data, size, limits)` takes a
buffer of module bytes, loads and verifies it, runs the entry function under a
bounded instruction/stack budget, and returns a status. For finer control,
`vl_module_load` / `vl_verify_module` / `vl_run_module` are the three stages it
composes (see `src/vm.h`, `src/module.h`, `src/verify.h`).

## Modules

Compiled programs live in `.qbc` files: a 20-byte header (magic `VLM1`,
version, constant and function counts, and the index of the entry function)
followed by the constant pool and the function table. Constants are 64-bit
ints, doubles, or inline strings; each function carries its arity, local count,
upvalue capture descriptors, and a run of bytecode. Everything is little-endian
and parsed through a bounded cursor, so a truncated or hostile file is rejected
rather than trusted.

Before a module runs it goes through a static verifier that checks every
opcode is known and fully contained, that operands index valid constants,
locals, upvalues, and functions, that jumps land on instruction boundaries, and
that the operand stack neither underflows nor exceeds a computed bound along any
path. The exact layout, opcode set, and operand encodings are in
[docs/BYTECODE.md](docs/BYTECODE.md).

## Building and testing

```
make          # build the library and the command-line tools
make test     # build and run the unit-test suite
```

The build is plain C11 and expects only a C compiler and `make`; it is
developed against `gcc -std=c11 -Wall -Wextra` with no warnings. `make SAN=1`
adds AddressSanitizer/UBSan for local checking.

## Tools

- **qasm** assembles the line-oriented text format described in
  [docs/ASSEMBLY.md](docs/ASSEMBLY.md) into a `.qbc` module.
- **qdis** disassembles a module into a readable listing of every function's
  bytecode - the fastest way to see what was emitted or to make sense of a
  verifier complaint.
- **qrun** loads, verifies, and runs a module and prints the result value.
- **qopt** runs a size-preserving peephole pass over a module and reports how
  many redundant instructions it neutralized.

A quick round trip:

```
printf '.func 0 0\npushint 6\npushint 7\nmul\nret\n.entry 0\n' > mul.qasm
./build/qasm mul.qasm mul.qbc
./build/qrun mul.qbc            # => 42
```

## Fuzzing

The harnesses in `fuzz/` drive the pipeline end to end on raw input:
`module_fuzzer` runs the whole loader -> verifier -> interpreter path, and
`verify_fuzzer` stops after verification. They build against libFuzzer (what
ClusterFuzzLite runs in CI via `.clusterfuzzlite/build.sh`), and
`fuzz/standalone_main.c` provides an engine-free driver that replays a saved
input so a crash reproduces locally, pairing with the page-guard debug
allocator (`make fuzz-local`).

## License

MIT - see [LICENSE](LICENSE).
