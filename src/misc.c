/*
 * misc.c - small shared helpers used across the Vellum VM.
 *
 * Provides human-readable names for status codes and opcodes, plus the
 * inline operand size table used by the verifier and disassembler.
 */
#include "internal.h"
#include "opcodes.h"

#include "vellum/error.h"

/* Returns a static, never-NULL short description of a status code. */
const char *vl_status_str(vl_status s)
{
    switch (s) {
    case VL_OK:              return "ok";
    case VL_ERR_IO:          return "io error";
    case VL_ERR_TRUNCATED:   return "truncated";
    case VL_ERR_BAD_MAGIC:   return "bad magic";
    case VL_ERR_BAD_VERSION: return "bad version";
    case VL_ERR_BAD_MODULE:  return "bad module";
    case VL_ERR_BAD_CONST:   return "bad const";
    case VL_ERR_BAD_FUNC:    return "bad func";
    case VL_ERR_BAD_CODE:    return "bad code";
    case VL_ERR_OOM:         return "out of memory";
    case VL_ERR_LIMIT:       return "limit exceeded";
    case VL_ERR_TYPE:        return "type error";
    case VL_ERR_STACK:       return "stack error";
    case VL_ERR_BUDGET:      return "budget exhausted";
    case VL_ERR_TRAP:        return "trap";
    case VL_ERR_CORRUPT:     return "corrupt";
    case VL_ERR_OVERFLOW:    return "overflow";
    case VL_ERR_UNSUPPORTED: return "unsupported";
    }
    return "unknown";
}

/* Returns the lowercase mnemonic for an opcode, or "?" if out of range. */
const char *vl_op_name(int op)
{
    switch (op) {
    case OP_NOP:         return "nop";
    case OP_PUSHK:       return "pushk";
    case OP_PUSHNIL:     return "pushnil";
    case OP_PUSHTRUE:    return "pushtrue";
    case OP_PUSHFALSE:   return "pushfalse";
    case OP_PUSHINT:     return "pushint";
    case OP_POP:         return "pop";
    case OP_DUP:         return "dup";
    case OP_SWAP:        return "swap";
    case OP_LOADLOCAL:   return "loadlocal";
    case OP_STORELOCAL:  return "storelocal";
    case OP_GETUPVAL:    return "getupval";
    case OP_SETUPVAL:    return "setupval";
    case OP_ADD:         return "add";
    case OP_SUB:         return "sub";
    case OP_MUL:         return "mul";
    case OP_DIV:         return "div";
    case OP_MOD:         return "mod";
    case OP_NEG:         return "neg";
    case OP_EQ:          return "eq";
    case OP_NE:          return "ne";
    case OP_LT:          return "lt";
    case OP_LE:          return "le";
    case OP_GT:          return "gt";
    case OP_GE:          return "ge";
    case OP_NOT:         return "not";
    case OP_JMP:         return "jmp";
    case OP_JMPIF:       return "jmpif";
    case OP_JMPIFNOT:    return "jmpifnot";
    case OP_NEWARRAY:    return "newarray";
    case OP_ARRGET:      return "arrget";
    case OP_ARRSET:      return "arrset";
    case OP_ARRPUSH:     return "arrpush";
    case OP_LEN:         return "len";
    case OP_NEWMAP:      return "newmap";
    case OP_MAPGET:      return "mapget";
    case OP_MAPSET:      return "mapset";
    case OP_CLOSURE:     return "closure";
    case OP_CALL:        return "call";
    case OP_TAILCALL:    return "tailcall";
    case OP_RET:         return "ret";
    case OP_CLOSEUPVALS: return "closeupvals";
    case OP_PRINT:       return "print";
    case OP_HALT:        return "halt";
    default:             return "?";
    }
}

/* Returns the inline operand size in bytes for an opcode (0 if none). */
int vl_op_operand_size(int op)
{
    switch (op) {
    case OP_PUSHK:
    case OP_LOADLOCAL:
    case OP_STORELOCAL:
    case OP_NEWARRAY:
    case OP_NEWMAP:
    case OP_CLOSURE:
    case OP_CLOSEUPVALS:
        return 2; /* u16 */
    case OP_PUSHINT:
        return 4; /* i32 */
    case OP_GETUPVAL:
    case OP_SETUPVAL:
    case OP_CALL:
    case OP_TAILCALL:
        return 1; /* u8 */
    case OP_JMP:
    case OP_JMPIF:
    case OP_JMPIFNOT:
        return 2; /* i16 */
    default:
        return 0;
    }
}
