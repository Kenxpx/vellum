"""
make_seeds.py - generate a valid seed corpus for the Vellum fuzz harnesses.

Each seed is a well-formed .qbc module the harness loads, verifies, and runs;
fuzzing mutates outward from them to reach deep interpreter state. Run:

    python scripts/make_seeds.py

Writes into fuzz/corpus/<harness>/ and refreshes fuzz/dictionary.txt.
"""
import os

from qasm import Func, Module


def s_arith():
    m = Module()
    e = Func(0, 0)
    m.entry = m.add_func(e)
    (e.emit("PUSHINT", 3).emit("PUSHINT", 4).emit("MUL")
     .emit("PUSHINT", 2).emit("ADD").emit("PRINT")
     .emit("PUSHINT", 0).emit("RET"))
    return m.build()


def s_array():
    m = Module()
    e = Func(0, 1)
    m.entry = m.add_func(e)
    (e.emit("NEWARRAY", 0).emit("STORELOCAL", 0)
     .emit("LOADLOCAL", 0).emit("PUSHINT", 10).emit("ARRPUSH")
     .emit("LOADLOCAL", 0).emit("PUSHINT", 20).emit("ARRPUSH")
     .emit("LOADLOCAL", 0).emit("LEN").emit("PRINT")
     .emit("PUSHINT", 0).emit("RET"))
    return m.build()


def s_map():
    m = Module()
    e = Func(0, 1)
    m.entry = m.add_func(e)
    (e.emit("NEWMAP", 0).emit("STORELOCAL", 0)
     .emit("LOADLOCAL", 0).emit("PUSHINT", 1).emit("PUSHINT", 100).emit("MAPSET")
     .emit("LOADLOCAL", 0).emit("PUSHINT", 2).emit("PUSHINT", 200).emit("MAPSET")
     .emit("LOADLOCAL", 0).emit("PUSHINT", 1).emit("MAPGET").emit("PRINT")
     .emit("PUSHINT", 0).emit("RET"))
    return m.build()


def s_closure():
    m = Module()
    e = Func(0, 1)
    reader = Func(0, 0, upvals=[(1, 0)])
    m.entry = m.add_func(e)
    ri = m.add_func(reader)
    reader.emit("GETUPVAL", 0).emit("RET")
    (e.emit("PUSHINT", 42).emit("STORELOCAL", 0)
     .emit("CLOSURE", ri).emit("CALL", 0).emit("PRINT")
     .emit("PUSHINT", 0).emit("RET"))
    return m.build()


def s_call():
    m = Module()
    e = Func(0, 0)
    dbl = Func(1, 1)
    m.entry = m.add_func(e)
    di = m.add_func(dbl)
    dbl.emit("LOADLOCAL", 0).emit("PUSHINT", 2).emit("MUL").emit("RET")
    (e.emit("CLOSURE", di).emit("PUSHINT", 21).emit("CALL", 1).emit("PRINT")
     .emit("PUSHINT", 0).emit("RET"))
    return m.build()


def s_loop():
    m = Module()
    e = Func(0, 1)
    m.entry = m.add_func(e)
    # local0 = 0; while local0 < 5: local0 += 1
    (e.emit("PUSHINT", 0).emit("STORELOCAL", 0)
     .label("top")
     .emit("LOADLOCAL", 0).emit("PUSHINT", 5).emit("LT").emit("JMPIFNOT", "end")
     .emit("LOADLOCAL", 0).emit("PUSHINT", 1).emit("ADD").emit("STORELOCAL", 0)
     .emit("JMP", "top")
     .label("end")
     .emit("LOADLOCAL", 0).emit("PRINT").emit("PUSHINT", 0).emit("RET"))
    return m.build()


DICTIONARY = '''\
# Vellum module fuzzing dictionary
magic="VLM1"
'''


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    print(f"  {len(data):4d}  {path}")


def main():
    seeds = {
        "arith.qbc": s_arith(),
        "array.qbc": s_array(),
        "map.qbc": s_map(),
        "closure.qbc": s_closure(),
        "call.qbc": s_call(),
        "loop.qbc": s_loop(),
    }
    for harness in ("module_fuzzer", "verify_fuzzer"):
        for name, data in seeds.items():
            write(os.path.join("fuzz", "corpus", harness, name), data)
    with open(os.path.join("fuzz", "dictionary.txt"), "w", newline="\n") as f:
        f.write(DICTIONARY)
    print("  wrote fuzz/dictionary.txt")


if __name__ == "__main__":
    main()
