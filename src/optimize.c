/*
 * optimize.c - a size-preserving peephole optimizer for loaded modules.
 *
 * Each function's bytecode is rewritten in place. The pass first maps the
 * instruction-start offsets by walking the code with vl_op_operand_size, then
 * examines adjacent instruction pairs and neutralizes provably-redundant ones
 * by overwriting their bytes with OP_NOP. Because every instruction keeps its
 * original length, jump offsets remain valid and the module still verifies.
 */
#include "optimize.h"

#include "internal.h"
#include "opcodes.h"

/* True if opcode leaves exactly one value on the stack with no side effects. */
static int is_pure_push(uint8_t op)
{
    switch (op) {
    case OP_PUSHK:
    case OP_PUSHNIL:
    case OP_PUSHTRUE:
    case OP_PUSHFALSE:
    case OP_PUSHINT:
        return 1;
    default:
        return 0;
    }
}

/*
 * True if the adjacent pair (a then b) computes to a no-op and may be replaced
 * by NOPs: a pure push immediately discarded by a pop, or a doubled negation or
 * logical-not.
 */
static int is_redundant_pair(uint8_t a, uint8_t b)
{
    if (is_pure_push(a) && b == OP_POP) {
        return 1;
    }
    if (a == OP_NEG && b == OP_NEG) {
        return 1;
    }
    if (a == OP_NOT && b == OP_NOT) {
        return 1;
    }
    return 0;
}

/*
 * Build the instruction-start byte-map for a function by decoding opcodes from
 * offset zero. Marks is_start[off] for every instruction boundary and stops
 * cleanly (without error) at the first instruction that would run past the end.
 */
static void map_starts(const vl_function *f, uint8_t *is_start)
{
    uint32_t ip = 0;

    while (ip < f->code_len) {
        uint8_t op = f->code[ip];
        int opsize = vl_op_operand_size(op);
        uint32_t next = ip + 1u + (uint32_t)opsize;

        if (next > f->code_len || next < ip) {
            break;
        }
        is_start[ip] = 1;
        ip = next;
    }
}

/*
 * Peephole-optimize a single function in place. On success *count holds the
 * number of instructions neutralized. Returns VL_ERR_OOM if the byte-map cannot
 * be allocated, otherwise VL_OK.
 */
static vl_status optimize_function(vl_function *f, uint32_t *count)
{
    uint8_t *is_start;
    uint32_t a;

    *count = 0;
    if (f->code_len == 0) {
        return VL_OK;
    }

    is_start = vl_calloc(f->code_len, 1);
    if (is_start == NULL) {
        return VL_ERR_OOM;
    }
    map_starts(f, is_start);

    a = 0;
    while (a < f->code_len && is_start[a]) {
        uint8_t op_a = f->code[a];
        int size_a = vl_op_operand_size(op_a);
        uint32_t b = a + 1u + (uint32_t)size_a;
        uint8_t op_b;
        int size_b;

        if (b >= f->code_len || !is_start[b]) {
            break;
        }
        op_b = f->code[b];
        if (is_redundant_pair(op_a, op_b)) {
            size_b = vl_op_operand_size(op_b);
            memset(f->code + a, OP_NOP, (size_t)1 + (size_t)size_a);
            memset(f->code + b, OP_NOP, (size_t)1 + (size_t)size_b);
            *count += 2;
            a = b + 1u + (uint32_t)size_b;
        } else {
            a = b;
        }
    }

    vl_free(is_start);
    return VL_OK;
}

/*
 * Peephole-optimize every function in the module in place. On return *removed
 * (if non-NULL) holds the total number of instructions neutralized to NOPs.
 */
vl_status vl_optimize_module(vl_module *m, uint32_t *removed)
{
    uint32_t total = 0;
    uint32_t i;

    for (i = 0; i < m->func_count; i++) {
        uint32_t count = 0;
        vl_status st = optimize_function(&m->funcs[i], &count);
        if (st != VL_OK) {
            return st;
        }
        total += count;
    }

    if (removed != NULL) {
        *removed = total;
    }
    return VL_OK;
}
