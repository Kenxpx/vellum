# The Vellum module format (.qbc) and bytecode

A Vellum module is a header, a constant pool, and a function table. All
multi-byte integers are **little-endian**; `varint` is unsigned LEB128. This is
the byte-level reference shared by the loader (`src/module.c`), the verifier
(`src/verify.c`), the disassembler, and the assembler.

## Header (20 bytes)

| off | type | field         | notes                              |
|-----|------|---------------|------------------------------------|
| 0   | u32  | `magic`       | `'Q','L','M','1'` = 0x314D4C51     |
| 4   | u16  | `version`     | must be 1                          |
| 6   | u16  | `flags`       | reserved, 0                        |
| 8   | u32  | `const_count` | <= VL_MAX_CONSTS                   |
| 12  | u32  | `func_count`  | >= 1, <= VL_MAX_FUNCS              |
| 16  | u32  | `entry`       | index of the entry function        |

## Constant pool — `const_count` entries

```
u8 kind
kind == 0 (INT):    i64 value
kind == 1 (REAL):   f64 value
kind == 2 (STRING): varint len (<= VL_MAX_STRING), then len bytes
```

## Function table — `func_count` entries

```
u8   arity
u16  num_locals          ; includes the arity arguments; <= VL_MAX_LOCALS
u8   num_upvals          ; <= VL_MAX_UPVALS
num_upvals x {
    u8 is_local          ; 1 = capture parent frame local, 0 = parent upvalue
    u8 index             ; local slot / parent upvalue index
}
u32  code_len            ; <= VL_MAX_CODE
u8   code[code_len]
```

## Instructions

Each instruction is a one-byte opcode followed by a fixed-width little-endian
operand:

| opcode              | operand   | stack effect (pop -> push) |
|---------------------|-----------|----------------------------|
| NOP                 | -         | 0 -> 0                     |
| PUSHK               | u16 kidx  | 0 -> 1                     |
| PUSHNIL/TRUE/FALSE  | -         | 0 -> 1                     |
| PUSHINT             | i32       | 0 -> 1                     |
| POP                 | -         | 1 -> 0                     |
| DUP                 | -         | 1 -> 2                     |
| SWAP                | -         | 2 -> 2                     |
| LOADLOCAL           | u16 slot  | 0 -> 1                     |
| STORELOCAL          | u16 slot  | 1 -> 0                     |
| GETUPVAL            | u8 idx    | 0 -> 1                     |
| SETUPVAL            | u8 idx    | 1 -> 0                     |
| ADD/SUB/MUL/DIV/MOD | -         | 2 -> 1                     |
| NEG/NOT             | -         | 1 -> 1                     |
| EQ/NE/LT/LE/GT/GE   | -         | 2 -> 1                     |
| JMP                 | i16 rel   | 0 -> 0                     |
| JMPIF/JMPIFNOT      | i16 rel   | 1 -> 0                     |
| NEWARRAY            | u16 n     | n -> 1                     |
| ARRGET              | -         | 2 -> 1                     |
| ARRSET              | -         | 3 -> 0                     |
| ARRPUSH             | -         | 2 -> 0                     |
| LEN                 | -         | 1 -> 1                     |
| NEWMAP              | u16 n     | 2n -> 1                    |
| MAPGET              | -         | 2 -> 1                     |
| MAPSET              | -         | 3 -> 0                     |
| CLOSURE             | u16 fidx  | 0 -> 1                     |
| CALL                | u8 argc   | argc+1 -> 1                |
| TAILCALL            | u8 argc   | argc+1 -> (returns)        |
| RET                 | -         | 1 -> (returns)             |
| CLOSEUPVALS         | u16 slot  | 0 -> 0                     |
| PRINT               | -         | 1 -> 0                     |
| HALT                | -         | 0 -> (stops)               |

Relative jumps (`i16 rel`) are measured from the byte **after** the operand and
must land on an instruction boundary within the same function. The verifier
walks each function once, checks every operand is in range and every jump lands
on a boundary, and computes the maximum operand-stack depth (rejecting any
function that could underflow or exceed it).
