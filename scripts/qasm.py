"""
qasm.py - a tiny assembler for Vellum modules (.qbc), matching docs/BYTECODE.md.

Build a module by adding constants and functions, emit instructions with the
`Func.emit` helpers (labels supported for jumps), then `Module.build()` returns
the exact bytes the loader reads. Used by the seed and proof-of-concept
generators.
"""
import struct

# Opcodes (must match src/opcodes.h enum order).
OPS = [
    "NOP", "PUSHK", "PUSHNIL", "PUSHTRUE", "PUSHFALSE", "PUSHINT",
    "POP", "DUP", "SWAP", "LOADLOCAL", "STORELOCAL", "GETUPVAL", "SETUPVAL",
    "ADD", "SUB", "MUL", "DIV", "MOD", "NEG",
    "EQ", "NE", "LT", "LE", "GT", "GE", "NOT",
    "JMP", "JMPIF", "JMPIFNOT",
    "NEWARRAY", "ARRGET", "ARRSET", "ARRPUSH", "LEN",
    "NEWMAP", "MAPGET", "MAPSET",
    "CLOSURE", "CALL", "TAILCALL", "RET", "CLOSEUPVALS",
    "PRINT", "HALT",
]
OP = {name: i for i, name in enumerate(OPS)}

# Inline operand encoding per opcode: 'u16' | 'i32' | 'u8' | 'i16' | None
OPERAND = {}
for n in ("PUSHK", "LOADLOCAL", "STORELOCAL", "NEWARRAY", "NEWMAP",
          "CLOSURE", "CLOSEUPVALS"):
    OPERAND[n] = "u16"
OPERAND["PUSHINT"] = "i32"
for n in ("GETUPVAL", "SETUPVAL", "CALL", "TAILCALL"):
    OPERAND[n] = "u8"
for n in ("JMP", "JMPIF", "JMPIFNOT"):
    OPERAND[n] = "i16"

QK_INT, QK_REAL, QK_STRING = 0, 1, 2


def _varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            break
    return bytes(out)


class Func:
    def __init__(self, arity=0, num_locals=0, upvals=None):
        self.arity = arity
        self.num_locals = num_locals
        self.upvals = list(upvals or [])  # list of (is_local, index)
        self.items = []  # ('op', name, operand) or ('label', name)

    def emit(self, name, operand=None):
        self.items.append(("op", name, operand))
        return self

    def label(self, name):
        self.items.append(("label", name, None))
        return self

    def assemble(self):
        # First pass: assign byte offsets, record label positions.
        off = 0
        labels = {}
        laid = []  # (name, operand, offset)
        for it in self.items:
            if it[0] == "label":
                labels[it[1]] = off
                continue
            name, operand = it[1], it[2]
            size = 1 + {None: 0, "u8": 1, "u16": 2, "i16": 2, "i32": 4}[
                OPERAND.get(name)]
            laid.append((name, operand, off))
            off += size
        code = bytearray()
        for name, operand, at in laid:
            code.append(OP[name])
            kind = OPERAND.get(name)
            if kind is None:
                continue
            if kind == "i16" and isinstance(operand, str):
                # jump to a label: rel from the byte after the operand
                target = labels[operand]
                rel = target - (at + 3)
                code += struct.pack("<h", rel)
            elif kind == "u16":
                code += struct.pack("<H", operand & 0xFFFF)
            elif kind == "i16":
                code += struct.pack("<h", operand)
            elif kind == "u8":
                code.append(operand & 0xFF)
            elif kind == "i32":
                code += struct.pack("<i", operand)
        return bytes(code)


class Module:
    def __init__(self):
        self.consts = []  # (kind, value)
        self.funcs = []   # Func
        self.entry = 0

    def const_int(self, v):
        self.consts.append((QK_INT, v))
        return len(self.consts) - 1

    def const_real(self, v):
        self.consts.append((QK_REAL, v))
        return len(self.consts) - 1

    def const_string(self, s):
        if isinstance(s, str):
            s = s.encode("utf-8")
        self.consts.append((QK_STRING, s))
        return len(self.consts) - 1

    def add_func(self, f):
        self.funcs.append(f)
        return len(self.funcs) - 1

    def build(self):
        out = bytearray()
        out += b"VLM1"
        out += struct.pack("<H", 1)        # version
        out += struct.pack("<H", 0)        # flags
        out += struct.pack("<I", len(self.consts))
        out += struct.pack("<I", len(self.funcs))
        out += struct.pack("<I", self.entry)
        for kind, val in self.consts:
            out.append(kind)
            if kind == QK_INT:
                out += struct.pack("<q", val)
            elif kind == QK_REAL:
                out += struct.pack("<d", val)
            else:
                out += _varint(len(val)) + val
        for f in self.funcs:
            code = f.assemble()
            out.append(f.arity & 0xFF)
            out += struct.pack("<H", f.num_locals & 0xFFFF)
            out.append(len(f.upvals) & 0xFF)
            for is_local, index in f.upvals:
                out.append(1 if is_local else 0)
                out.append(index & 0xFF)
            out += struct.pack("<I", len(code))
            out += code
        return bytes(out)
