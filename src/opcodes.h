/*
 * opcodes.h - the Vellum bytecode instruction set.
 *
 * Each instruction is a one-byte opcode optionally followed by a fixed-width
 * little-endian operand (see vl_op_operand_size). Operands index the constant
 * pool, locals, upvalues, or functions, or encode a small immediate or a
 * relative jump. The verifier (verify.c) checks operands and the stack effect
 * of every instruction before a module is allowed to run.
 */
#ifndef VELLUM_OPCODES_H
#define VELLUM_OPCODES_H

enum vl_op {
    OP_NOP = 0,

    OP_PUSHK,      /* u16 const index -> push constant                     */
    OP_PUSHNIL,    /* -> push nil                                          */
    OP_PUSHTRUE,   /* -> push true                                         */
    OP_PUSHFALSE,  /* -> push false                                        */
    OP_PUSHINT,    /* i32 immediate -> push integer                        */

    OP_POP,        /* pop 1                                                */
    OP_DUP,        /* duplicate top                                        */
    OP_SWAP,       /* swap top two                                         */

    OP_LOADLOCAL,  /* u16 slot -> push locals[slot]                        */
    OP_STORELOCAL, /* u16 slot -> locals[slot] = pop                       */
    OP_GETUPVAL,   /* u8 idx  -> push upvalue                              */
    OP_SETUPVAL,   /* u8 idx  -> upvalue = pop                             */

    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_NOT,

    OP_JMP,        /* i16 rel                                              */
    OP_JMPIF,      /* i16 rel -> pop cond, jump if truthy                  */
    OP_JMPIFNOT,   /* i16 rel -> pop cond, jump if falsey                  */

    OP_NEWARRAY,   /* u16 n -> pop n values, push array                    */
    OP_ARRGET,     /* pop idx, arr -> push arr[idx]                        */
    OP_ARRSET,     /* pop val, idx, arr -> arr[idx] = val                  */
    OP_ARRPUSH,    /* pop val, arr -> arr.push(val)                        */
    OP_LEN,        /* pop container -> push length                         */

    OP_NEWMAP,     /* u16 n -> pop 2n values (k,v)..., push map            */
    OP_MAPGET,     /* pop key, map -> push map[key]                        */
    OP_MAPSET,     /* pop val, key, map -> map[key] = val                  */

    OP_CLOSURE,    /* u16 func idx -> push closure capturing upvalues      */
    OP_CALL,       /* u8 argc -> call closure with argc args              */
    OP_TAILCALL,   /* u8 argc -> tail call (reuses the current frame)     */
    OP_RET,        /* return top of stack                                  */
    OP_CLOSEUPVALS,/* u16 slot -> close open upvalues at slots >= slot     */

    OP_PRINT,      /* pop and record (observable sink)                     */
    OP_HALT,       /* stop execution                                       */

    OP__COUNT
};

/* Name of an opcode, or "?" if out of range. */
const char *vl_op_name(int op);

/* Inline operand size in bytes for an opcode (0 if none). */
int vl_op_operand_size(int op);

#endif /* VELLUM_OPCODES_H */
