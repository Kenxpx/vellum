/*
 * disasm.c - human-readable disassembly of a loaded module. Renders the header,
 * the constant pool, and every function's bytecode with resolved operands. All
 * reads into function code are bounds-checked so a malformed (but loaded) module
 * can never make the disassembler read past code_len.
 */
#include "disasm.h"

#include <inttypes.h>
#include <stdio.h>

#include "internal.h"
#include "module.h"
#include "opcodes.h"

/* Print one string byte to out, escaping quotes, backslashes and non-printables. */
static void vl_disasm_string_byte(FILE *out, uint8_t c) {
    switch (c) {
    case '"':
        fputs("\\\"", out);
        break;
    case '\\':
        fputs("\\\\", out);
        break;
    case '\n':
        fputs("\\n", out);
        break;
    case '\t':
        fputs("\\t", out);
        break;
    case '\r':
        fputs("\\r", out);
        break;
    default:
        if (c >= 0x20 && c < 0x7F) {
            fputc((int)c, out);
        } else {
            fprintf(out, "\\x%02X", (unsigned)c);
        }
        break;
    }
}

/* Print a constant's kind and value ("INT 7", "REAL 3.14", "STRING \"hi\""). */
static void vl_disasm_const(FILE *out, const vl_const *k) {
    switch (k->kind) {
    case QK_INT:
        fprintf(out, "INT %" PRId64, k->as.i);
        break;
    case QK_REAL:
        fprintf(out, "REAL %g", k->as.d);
        break;
    case QK_STRING: {
        uint32_t j;
        fputs("STRING \"", out);
        for (j = 0; j < k->as.s.len; j++) {
            vl_disasm_string_byte(out, (uint8_t)k->as.s.data[j]);
        }
        fputc('"', out);
        break;
    }
    default:
        fprintf(out, "?kind=%u", (unsigned)k->kind);
        break;
    }
}

/* Disassemble one function's bytecode to out. */
vl_status vl_disasm_function(const vl_module *m, const vl_function *f, FILE *out) {
    uint32_t index;
    uint32_t ip;

    if (m == NULL || f == NULL || out == NULL) {
        return VL_ERR_IO;
    }

    index = (uint32_t)(f - m->funcs);
    fprintf(out, "fn #%" PRIu32 " arity=%u locals=%u upvals=%u code=%" PRIu32 "\n",
            index, (unsigned)f->arity, (unsigned)f->num_locals,
            (unsigned)f->num_upvals, f->code_len);

    ip = 0;
    while (ip < f->code_len) {
        uint32_t off = ip;
        int op = f->code[ip];
        int size = vl_op_operand_size(op);
        const char *name = vl_op_name(op);

        /* Not enough bytes left for this instruction's operand. */
        if (size < 0 || (uint32_t)size > f->code_len - ip - 1) {
            fprintf(out, "  %04" PRIu32 ": %s <truncated>\n", off, name);
            break;
        }

        fprintf(out, "  %04" PRIu32 ": %s", off, name);

        if (size > 0) {
            const uint8_t *p = f->code + ip + 1;
            switch (op) {
            case OP_PUSHINT: {
                /* i32 immediate */
                int32_t v = (int32_t)vl_load_u32le(p);
                fprintf(out, " %" PRId32, v);
                break;
            }
            case OP_JMP:
            case OP_JMPIF:
            case OP_JMPIFNOT: {
                /* i16 relative; render the absolute target. */
                int16_t rel = (int16_t)vl_load_u16le(p);
                /* Target is measured from the byte after the operand. */
                int64_t target = (int64_t)ip + 1 + size + rel;
                fprintf(out, " %" PRId64, target);
                break;
            }
            case OP_PUSHK: {
                /* u16 constant index; also show the constant value. */
                uint16_t kidx = vl_load_u16le(p);
                const vl_const *k = vl_module_const(m, kidx);
                fprintf(out, " %u", (unsigned)kidx);
                if (k != NULL) {
                    fputs(" ; ", out);
                    vl_disasm_const(out, k);
                }
                break;
            }
            default:
                /* Remaining operands are plain unsigned immediates. */
                if (size == 1) {
                    fprintf(out, " %u", (unsigned)p[0]);
                } else if (size == 2) {
                    fprintf(out, " %u", (unsigned)vl_load_u16le(p));
                } else if (size == 4) {
                    fprintf(out, " %" PRIu32, vl_load_u32le(p));
                }
                break;
            }
        }

        fputc('\n', out);
        ip += 1 + (uint32_t)size;
    }

    return VL_OK;
}

/* Disassemble the whole module (header, constants, functions) to out. */
vl_status vl_disasm_module(const vl_module *m, FILE *out) {
    uint32_t i;

    if (m == NULL || out == NULL) {
        return VL_ERR_IO;
    }

    fprintf(out, "module v%u consts=%" PRIu32 " funcs=%" PRIu32 " entry=%" PRIu32 "\n",
            (unsigned)m->version, m->const_count, m->func_count, m->entry);

    for (i = 0; i < m->const_count; i++) {
        fprintf(out, "  %" PRIu32 ": ", i);
        vl_disasm_const(out, &m->consts[i]);
        fputc('\n', out);
    }

    for (i = 0; i < m->func_count; i++) {
        vl_status s = vl_disasm_function(m, &m->funcs[i], out);
        if (s != VL_OK) {
            return s;
        }
    }

    return VL_OK;
}
