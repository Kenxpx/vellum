/*
 * verify.c - single-pass static bytecode verifier.
 *
 * Each function is checked in three linked steps: a linear scan that confirms
 * every opcode is known and fully contained and records instruction-start
 * offsets; a stack pass that, in address order, validates operands and jump
 * targets and propagates an abstract operand-stack depth (rejecting underflow
 * and inconsistent merges); and the computation of the peak depth used to size
 * the VM frame. Arbitrary jumps make full dataflow impractical, so the depth
 * check is deliberately conservative: the depth recorded when an instruction is
 * first reached must match every later arrival.
 */
#include "internal.h"
#include "module.h"
#include "opcodes.h"
#include "verify.h"

/* Successor shape of an instruction for depth propagation. */
enum {
    SUCC_FALL = 0, /* single fallthrough successor        */
    SUCC_JMP,      /* unconditional branch (target only)  */
    SUCC_COND,     /* conditional branch (target + fall)  */
    SUCC_TERM      /* terminates the path (RET/HALT/TAIL)  */
};

/*
 * Record an incoming operand-stack depth at an instruction offset. The first
 * arrival sets it; every later arrival must agree or the function is rejected.
 */
static vl_status set_depth(int32_t *depth_at, uint32_t at, int32_t val) {
    if (depth_at[at] < 0) {
        depth_at[at] = val;
        return VL_OK;
    }
    if (depth_at[at] != val) {
        return VL_ERR_BAD_CODE;
    }
    return VL_OK;
}

/*
 * Linear scan: decode instructions from offset 0, rejecting unknown opcodes and
 * any instruction whose operand runs past the end, and mark each start offset.
 */
static vl_status verify_scan(const vl_function *f, uint8_t *is_start) {
    uint32_t ip = 0;
    while (ip < f->code_len) {
        uint8_t op = f->code[ip];
        if (op >= OP__COUNT) {
            return VL_ERR_BAD_CODE;
        }
        int opsize = vl_op_operand_size(op);
        size_t end = (size_t)ip + 1u + (size_t)opsize;
        if (end > f->code_len) {
            return VL_ERR_BAD_CODE;
        }
        is_start[ip] = 1;
        ip = (uint32_t)end;
    }
    return VL_OK;
}

/*
 * Stack pass: walk instructions in address order, validate each operand against
 * the module and function limits, check branch targets land on instruction
 * boundaries, and propagate operand-stack depth. Stores the peak depth in *max.
 */
static vl_status verify_stack(const vl_module *m, const vl_function *f,
                              const uint8_t *is_start, int32_t *depth_at,
                              uint32_t *max) {
    uint32_t peak = 0;
    uint32_t ip = 0;
    vl_status st;

    while (ip < f->code_len) {
        int32_t cur = depth_at[ip];
        uint8_t op = f->code[ip];
        int opsize = vl_op_operand_size(op);
        uint32_t next = ip + 1u + (uint32_t)opsize;
        const uint8_t *operand = f->code + (size_t)ip + 1u;
        int32_t pops = 0, pushes = 0;
        int mode = SUCC_FALL;
        int32_t nd;

        /* Unreachable in this conservative address-order scheme: skip it. */
        if (cur < 0) {
            ip = next;
            continue;
        }
        if ((uint32_t)cur > peak) {
            peak = (uint32_t)cur;
        }

        switch (op) {
        case OP_NOP:
            break;
        case OP_PUSHK: {
            uint16_t k = vl_load_u16le(operand);
            if (k >= m->const_count) {
                return VL_ERR_BAD_CODE;
            }
            pushes = 1;
            break;
        }
        case OP_PUSHNIL:
        case OP_PUSHTRUE:
        case OP_PUSHFALSE:
        case OP_PUSHINT:
            pushes = 1;
            break;
        case OP_POP:
            pops = 1;
            break;
        case OP_DUP:
            pops = 1;
            pushes = 2;
            break;
        case OP_SWAP:
            pops = 2;
            pushes = 2;
            break;
        case OP_LOADLOCAL: {
            uint16_t slot = vl_load_u16le(operand);
            if (slot >= f->num_locals) {
                return VL_ERR_BAD_CODE;
            }
            pushes = 1;
            break;
        }
        case OP_STORELOCAL: {
            uint16_t slot = vl_load_u16le(operand);
            if (slot >= f->num_locals) {
                return VL_ERR_BAD_CODE;
            }
            pops = 1;
            break;
        }
        case OP_GETUPVAL: {
            uint8_t idx = operand[0];
            if (idx >= f->num_upvals) {
                return VL_ERR_BAD_CODE;
            }
            pushes = 1;
            break;
        }
        case OP_SETUPVAL: {
            uint8_t idx = operand[0];
            if (idx >= f->num_upvals) {
                return VL_ERR_BAD_CODE;
            }
            pops = 1;
            break;
        }
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
            pops = 2;
            pushes = 1;
            break;
        case OP_NEG:
            pops = 1;
            pushes = 1;
            break;
        case OP_EQ:
        case OP_NE:
        case OP_LT:
        case OP_LE:
        case OP_GT:
        case OP_GE:
            pops = 2;
            pushes = 1;
            break;
        case OP_NOT:
            pops = 1;
            pushes = 1;
            break;
        case OP_JMP:
            mode = SUCC_JMP;
            break;
        case OP_JMPIF:
        case OP_JMPIFNOT:
            pops = 1;
            mode = SUCC_COND;
            break;
        case OP_NEWARRAY: {
            uint16_t n = vl_load_u16le(operand);
            pops = (int32_t)n;
            pushes = 1;
            break;
        }
        case OP_ARRGET:
            pops = 2;
            pushes = 1;
            break;
        case OP_ARRSET:
            pops = 3;
            break;
        case OP_ARRPUSH:
            pops = 2;
            break;
        case OP_LEN:
            pops = 1;
            pushes = 1;
            break;
        case OP_NEWMAP: {
            uint16_t n = vl_load_u16le(operand);
            pops = 2 * (int32_t)n;
            pushes = 1;
            break;
        }
        case OP_MAPGET:
            pops = 2;
            pushes = 1;
            break;
        case OP_MAPSET:
            pops = 3;
            break;
        case OP_CLOSURE: {
            uint16_t fidx = vl_load_u16le(operand);
            if (fidx >= m->func_count) {
                return VL_ERR_BAD_CODE;
            }
            pushes = 1;
            break;
        }
        case OP_CALL: {
            uint8_t argc = operand[0];
            if (argc > 64) {
                return VL_ERR_BAD_CODE;
            }
            pops = (int32_t)argc + 1;
            pushes = 1;
            break;
        }
        case OP_TAILCALL: {
            uint8_t argc = operand[0];
            if (argc > 64) {
                return VL_ERR_BAD_CODE;
            }
            pops = (int32_t)argc + 1;
            mode = SUCC_TERM;
            break;
        }
        case OP_RET:
            pops = 1;
            mode = SUCC_TERM;
            break;
        case OP_CLOSEUPVALS:
            break;
        case OP_PRINT:
            pops = 1;
            break;
        case OP_HALT:
            mode = SUCC_TERM;
            break;
        default:
            return VL_ERR_BAD_CODE;
        }

        if (cur < pops) {
            return VL_ERR_BAD_CODE;
        }
        nd = cur - pops + pushes;
        if ((uint32_t)nd > peak) {
            peak = (uint32_t)nd;
        }

        if (mode == SUCC_JMP || mode == SUCC_COND) {
            int32_t rel = (int16_t)vl_load_u16le(operand);
            int64_t target = (int64_t)next + rel;
            if (target < 0 || (uint64_t)target >= f->code_len) {
                return VL_ERR_BAD_CODE;
            }
            if (!is_start[target]) {
                return VL_ERR_BAD_CODE;
            }
            st = set_depth(depth_at, (uint32_t)target, nd);
            if (st != VL_OK) {
                return st;
            }
        }
        if (mode == SUCC_FALL || mode == SUCC_COND) {
            if (next < f->code_len) {
                st = set_depth(depth_at, next, nd);
                if (st != VL_OK) {
                    return st;
                }
            }
        }

        ip = next;
    }

    *max = peak;
    return VL_OK;
}

/*
 * Verify one function's bytecode and, on success, report the peak operand-stack
 * depth. Allocates and frees the two temporary per-byte maps.
 */
vl_status vl_verify_function(const vl_module *m, const vl_function *f,
                             uint32_t *max_stack) {
    uint8_t *is_start;
    int32_t *depth_at;
    size_t dsz;
    uint32_t max = 0;
    vl_status st;

    if (max_stack) {
        *max_stack = 0;
    }
    /* Degenerate empty function: nothing to execute, no stack used. */
    if (f->code_len == 0) {
        return VL_OK;
    }

    is_start = vl_calloc(f->code_len, sizeof(uint8_t));
    if (!is_start) {
        return VL_ERR_OOM;
    }
    st = verify_scan(f, is_start);
    if (st != VL_OK) {
        vl_free(is_start);
        return st;
    }

    if (!vl_size_mul(f->code_len, sizeof(int32_t), &dsz)) {
        vl_free(is_start);
        return VL_ERR_OOM;
    }
    depth_at = vl_malloc(dsz);
    if (!depth_at) {
        vl_free(is_start);
        return VL_ERR_OOM;
    }
    /* 0xFF bytes -> every int32 slot is -1 (unset). */
    memset(depth_at, 0xFF, dsz);
    depth_at[0] = 0;

    st = verify_stack(m, f, is_start, depth_at, &max);

    vl_free(depth_at);
    vl_free(is_start);

    if (st != VL_OK) {
        return st;
    }
    if (max_stack) {
        *max_stack = max;
    }
    return VL_OK;
}

/*
 * Verify every function in the module, returning the first failure. The
 * per-function peak-depth results are not needed here (the VM recomputes them
 * when it loads a frame), so they are discarded.
 */
vl_status vl_verify_module(const vl_module *m) {
    uint32_t i;

    if (!m) {
        return VL_ERR_BAD_CODE;
    }
    for (i = 0; i < m->func_count; i++) {
        uint32_t max_stack;
        vl_status st = vl_verify_function(m, &m->funcs[i], &max_stack);
        if (st != VL_OK) {
            return st;
        }
    }
    return VL_OK;
}
