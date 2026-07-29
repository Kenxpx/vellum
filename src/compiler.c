/*
 * compiler.c - lower a parsed Vellum program to a .qbc module.
 *
 * Each source function becomes a VM function in program order (so a function's
 * source index is also its builder/module index). Parameters occupy the first
 * local slots; every `let` allocates the next slot with a monotonically
 * increasing counter (slots are never reused). A function's num_locals is the
 * parameter count plus the number of `let` bindings anywhere in its body.
 *
 * Codegen for a function collects instructions into an internal list (as the
 * text assembler does): 0-operand ops, immediate-operand ops, and label-target
 * jumps. Once the whole function is collected, each instruction's byte offset is
 * known, so every jump's label resolves to a relative i16 and the instructions
 * are emitted through the builder. The function named "main" (arity 0) is the
 * module entry, and the emitted module always passes vl_verify_module.
 */
#include "compiler.h"

#include <stdio.h>

#include "ast.h"
#include "builder.h"
#include "internal.h"
#include "module.h"
#include "opcodes.h"
#include "parser.h"

/* One collected instruction awaiting emission into the builder. */
typedef struct {
    uint8_t op;      /* opcode                                              */
    int size;        /* inline operand size in bytes (0/1/2/4)              */
    int is_jump;     /* 1 if the operand is a relative jump to a label      */
    int label;       /* target label id (when is_jump), else -1             */
    int64_t imm;     /* immediate operand value (when !is_jump)             */
    uint32_t offset; /* byte offset of this instruction within the function */
} cins;

/* One local binding: a borrowed name and the slot it occupies. */
typedef struct {
    const char *name; /* borrowed from the AST (not owned)                  */
    int slot;
} cbind;

/* Module-wide compile state. */
typedef struct {
    vl_builder *b;
    const vl_program *p;
    char *err;
    size_t errcap;
} cstate;

/* Per-function codegen state. */
typedef struct {
    cstate *cs;
    int func;            /* builder function index                          */
    cins *ins;
    size_t nins, cap_ins;
    int *labels;         /* labels[id] = instruction index it precedes      */
    size_t nlabels, cap_labels;
    cbind *scope;
    size_t nscope, cap_scope;
    int next_slot;       /* next free local slot                            */
    int num_locals;      /* total local slots for this function             */
} fctx;

/* Write a short message into err[0,errcap) when a buffer was provided. */
static void set_cerr(cstate *cs, const char *msg) {
    if (cs->err != NULL && cs->errcap > 0) {
        snprintf(cs->err, cs->errcap, "%s", msg);
    }
}

/* Write a message with an embedded name into err (truncated to fit). */
static void set_cerr_name(cstate *cs, const char *fmt, const char *name) {
    if (cs->err != NULL && cs->errcap > 0) {
        snprintf(cs->err, cs->errcap, fmt, name);
    }
}

/* Recursively count `let` bindings within a statement (0 for NULL). */
static int count_lets(const vl_stmt *s) {
    int n = 0;
    int i;
    if (s == NULL) {
        return 0;
    }
    switch (s->kind) {
    case ST_LET:
        n = 1;
        break;
    case ST_IF:
        n = count_lets(s->as.if_s.then_s);
        if (s->as.if_s.else_s != NULL) {
            n += count_lets(s->as.if_s.else_s);
        }
        break;
    case ST_WHILE:
        n = count_lets(s->as.while_s.body);
        break;
    case ST_BLOCK:
        for (i = 0; i < s->as.block.n; i++) {
            n += count_lets(s->as.block.items[i]);
        }
        break;
    default:
        n = 0;
        break;
    }
    return n;
}

/* Ensure room for one more collected instruction. Returns a vl_status. */
static vl_status reserve_ins(fctx *fc) {
    if (fc->nins == fc->cap_ins) {
        size_t ncap = fc->cap_ins ? fc->cap_ins * 2 : 16;
        size_t bytes;
        cins *p;
        if (!vl_size_mul(ncap, sizeof(cins), &bytes)) {
            return VL_ERR_OVERFLOW;
        }
        p = vl_realloc(fc->ins, bytes);
        if (p == NULL) {
            return VL_ERR_OOM;
        }
        fc->ins = p;
        fc->cap_ins = ncap;
    }
    return VL_OK;
}

/* Ensure room for one more label. Returns a vl_status. */
static vl_status reserve_labels(fctx *fc) {
    if (fc->nlabels == fc->cap_labels) {
        size_t ncap = fc->cap_labels ? fc->cap_labels * 2 : 16;
        size_t bytes;
        int *p;
        if (!vl_size_mul(ncap, sizeof(int), &bytes)) {
            return VL_ERR_OVERFLOW;
        }
        p = vl_realloc(fc->labels, bytes);
        if (p == NULL) {
            return VL_ERR_OOM;
        }
        fc->labels = p;
        fc->cap_labels = ncap;
    }
    return VL_OK;
}

/* Ensure room for one more local binding. Returns a vl_status. */
static vl_status reserve_scope(fctx *fc) {
    if (fc->nscope == fc->cap_scope) {
        size_t ncap = fc->cap_scope ? fc->cap_scope * 2 : 16;
        size_t bytes;
        cbind *p;
        if (!vl_size_mul(ncap, sizeof(cbind), &bytes)) {
            return VL_ERR_OVERFLOW;
        }
        p = vl_realloc(fc->scope, bytes);
        if (p == NULL) {
            return VL_ERR_OOM;
        }
        fc->scope = p;
        fc->cap_scope = ncap;
    }
    return VL_OK;
}

/* Append a collected instruction (op, immediate, and optional jump label). */
static vl_status emit_ins(fctx *fc, uint8_t op, int64_t imm, int label) {
    cins *in;
    vl_status rc = reserve_ins(fc);
    if (rc != VL_OK) {
        set_cerr(fc->cs, "out of memory");
        return rc;
    }
    in = &fc->ins[fc->nins++];
    in->op = op;
    in->size = vl_op_operand_size(op);
    in->is_jump = (op == OP_JMP || op == OP_JMPIF || op == OP_JMPIFNOT);
    in->label = label;
    in->imm = imm;
    in->offset = 0;
    return VL_OK;
}

/* Append a 0-operand instruction. */
static vl_status emit0(fctx *fc, uint8_t op) {
    return emit_ins(fc, op, 0, -1);
}

/* Append an instruction with an immediate operand. */
static vl_status emit_arg(fctx *fc, uint8_t op, int64_t imm) {
    return emit_ins(fc, op, imm, -1);
}

/* Append a jump instruction targeting a label id. */
static vl_status emit_jmp(fctx *fc, uint8_t op, int label) {
    return emit_ins(fc, op, 0, label);
}

/* Allocate a fresh, unplaced label id. Returns a vl_status; id via *out. */
static vl_status new_label(fctx *fc, int *out) {
    vl_status rc = reserve_labels(fc);
    if (rc != VL_OK) {
        set_cerr(fc->cs, "out of memory");
        return rc;
    }
    fc->labels[fc->nlabels] = -1;
    *out = (int)fc->nlabels;
    fc->nlabels++;
    return VL_OK;
}

/* Place a label at the current instruction position. */
static void place_label(fctx *fc, int id) {
    fc->labels[id] = (int)fc->nins;
}

/* Append a local binding for name occupying the given slot. */
static vl_status scope_append(fctx *fc, const char *name, int slot) {
    vl_status rc = reserve_scope(fc);
    if (rc != VL_OK) {
        set_cerr(fc->cs, "out of memory");
        return rc;
    }
    fc->scope[fc->nscope].name = name;
    fc->scope[fc->nscope].slot = slot;
    fc->nscope++;
    return VL_OK;
}

/* Resolve name to its most recent local slot. Returns 1 and stores *slot. */
static int scope_find(fctx *fc, const char *name, int *slot) {
    size_t i = fc->nscope;
    while (i > 0) {
        i--;
        if (strcmp(fc->scope[i].name, name) == 0) {
            *slot = fc->scope[i].slot;
            return 1;
        }
    }
    return 0;
}

/* Find a function by name; returns 1 and stores its index and arity. */
static int find_func(cstate *cs, const char *name, int *idx, int *arity) {
    int i;
    for (i = 0; i < cs->p->nfuncs; i++) {
        if (strcmp(cs->p->funcs[i]->name, name) == 0) {
            *idx = i;
            *arity = cs->p->funcs[i]->nparams;
            return 1;
        }
    }
    return 0;
}

/* Map a binary/unary operator kind to its opcode; returns OP__COUNT if none. */
static uint8_t binop_opcode(int opk) {
    switch (opk) {
    case OPK_ADD: return OP_ADD;
    case OPK_SUB: return OP_SUB;
    case OPK_MUL: return OP_MUL;
    case OPK_DIV: return OP_DIV;
    case OPK_MOD: return OP_MOD;
    case OPK_EQ:  return OP_EQ;
    case OPK_NE:  return OP_NE;
    case OPK_LT:  return OP_LT;
    case OPK_LE:  return OP_LE;
    case OPK_GT:  return OP_GT;
    case OPK_GE:  return OP_GE;
    default:      return OP__COUNT;
    }
}

static vl_status gen_expr(fctx *fc, const vl_expr *e);
static vl_status gen_stmt(fctx *fc, const vl_stmt *s);

/* Emit short-circuit `and`: value is r when l is truthy, else false. */
static vl_status gen_and(fctx *fc, const vl_expr *e) {
    int lfalse, lend;
    vl_status rc;
    if ((rc = new_label(fc, &lfalse)) != VL_OK) return rc;
    if ((rc = new_label(fc, &lend)) != VL_OK) return rc;
    if ((rc = gen_expr(fc, e->as.binary.l)) != VL_OK) return rc;
    if ((rc = emit_jmp(fc, OP_JMPIFNOT, lfalse)) != VL_OK) return rc;
    if ((rc = gen_expr(fc, e->as.binary.r)) != VL_OK) return rc;
    if ((rc = emit_jmp(fc, OP_JMP, lend)) != VL_OK) return rc;
    place_label(fc, lfalse);
    if ((rc = emit0(fc, OP_PUSHFALSE)) != VL_OK) return rc;
    place_label(fc, lend);
    return VL_OK;
}

/* Emit short-circuit `or`: value is r when l is falsey, else true. */
static vl_status gen_or(fctx *fc, const vl_expr *e) {
    int ltrue, lend;
    vl_status rc;
    if ((rc = new_label(fc, &ltrue)) != VL_OK) return rc;
    if ((rc = new_label(fc, &lend)) != VL_OK) return rc;
    if ((rc = gen_expr(fc, e->as.binary.l)) != VL_OK) return rc;
    if ((rc = emit_jmp(fc, OP_JMPIF, ltrue)) != VL_OK) return rc;
    if ((rc = gen_expr(fc, e->as.binary.r)) != VL_OK) return rc;
    if ((rc = emit_jmp(fc, OP_JMP, lend)) != VL_OK) return rc;
    place_label(fc, ltrue);
    if ((rc = emit0(fc, OP_PUSHTRUE)) != VL_OK) return rc;
    place_label(fc, lend);
    return VL_OK;
}

/* Emit a call: resolve the callee name, push the closure, then args, CALL. */
static vl_status gen_call(fctx *fc, const vl_expr *e) {
    const vl_expr *callee = e->as.call.callee;
    int fidx, arity, i;
    vl_status rc;

    if (callee == NULL || callee->kind != EX_IDENT) {
        set_cerr(fc->cs, "call target must be a function name");
        return VL_ERR_BAD_CODE;
    }
    if (!find_func(fc->cs, callee->as.ident.name, &fidx, &arity)) {
        set_cerr_name(fc->cs, "unknown function '%s'", callee->as.ident.name);
        return VL_ERR_BAD_CODE;
    }
    if (e->as.call.nargs != arity) {
        set_cerr_name(fc->cs, "wrong argument count for '%s'",
                      callee->as.ident.name);
        return VL_ERR_BAD_CODE;
    }
    if (e->as.call.nargs < 0 || e->as.call.nargs > 64) {
        set_cerr(fc->cs, "too many call arguments");
        return VL_ERR_BAD_CODE;
    }
    if ((rc = emit_arg(fc, OP_CLOSURE, fidx)) != VL_OK) return rc;
    for (i = 0; i < e->as.call.nargs; i++) {
        if ((rc = gen_expr(fc, e->as.call.args[i])) != VL_OK) return rc;
    }
    return emit_arg(fc, OP_CALL, e->as.call.nargs);
}

/* Emit code for an expression, leaving exactly one value on the stack. */
static vl_status gen_expr(fctx *fc, const vl_expr *e) {
    vl_status rc;
    int i;

    if (e == NULL) {
        set_cerr(fc->cs, "missing expression");
        return VL_ERR_BAD_CODE;
    }

    switch (e->kind) {
    case EX_NIL:
        return emit0(fc, OP_PUSHNIL);
    case EX_BOOL:
        return emit0(fc, e->as.b ? OP_PUSHTRUE : OP_PUSHFALSE);
    case EX_INT:
        if (e->as.i >= INT32_MIN && e->as.i <= INT32_MAX) {
            return emit_arg(fc, OP_PUSHINT, e->as.i);
        } else {
            int k = vl_builder_const_int(fc->cs->b, e->as.i);
            if (k < 0) {
                set_cerr(fc->cs, "out of memory");
                return VL_ERR_OOM;
            }
            return emit_arg(fc, OP_PUSHK, k);
        }
    case EX_REAL: {
        int k = vl_builder_const_real(fc->cs->b, e->as.d);
        if (k < 0) {
            set_cerr(fc->cs, "out of memory");
            return VL_ERR_OOM;
        }
        return emit_arg(fc, OP_PUSHK, k);
    }
    case EX_STR: {
        int k = vl_builder_const_string(fc->cs->b, e->as.str.data,
                                        e->as.str.len);
        if (k < 0) {
            set_cerr(fc->cs, "out of memory");
            return VL_ERR_OOM;
        }
        return emit_arg(fc, OP_PUSHK, k);
    }
    case EX_IDENT: {
        int slot;
        if (!scope_find(fc, e->as.ident.name, &slot)) {
            set_cerr_name(fc->cs, "unknown variable '%s'", e->as.ident.name);
            return VL_ERR_BAD_CODE;
        }
        return emit_arg(fc, OP_LOADLOCAL, slot);
    }
    case EX_UNARY:
        if ((rc = gen_expr(fc, e->as.unary.e)) != VL_OK) return rc;
        if (e->as.unary.op == OPK_NEG) {
            return emit0(fc, OP_NEG);
        } else if (e->as.unary.op == OPK_NOT) {
            return emit0(fc, OP_NOT);
        }
        set_cerr(fc->cs, "bad unary operator");
        return VL_ERR_BAD_CODE;
    case EX_BINARY: {
        uint8_t op;
        if (e->as.binary.op == OPK_AND) {
            return gen_and(fc, e);
        }
        if (e->as.binary.op == OPK_OR) {
            return gen_or(fc, e);
        }
        op = binop_opcode(e->as.binary.op);
        if (op == OP__COUNT) {
            set_cerr(fc->cs, "bad binary operator");
            return VL_ERR_BAD_CODE;
        }
        if ((rc = gen_expr(fc, e->as.binary.l)) != VL_OK) return rc;
        if ((rc = gen_expr(fc, e->as.binary.r)) != VL_OK) return rc;
        return emit0(fc, op);
    }
    case EX_CALL:
        return gen_call(fc, e);
    case EX_INDEX:
        if ((rc = gen_expr(fc, e->as.index.base)) != VL_OK) return rc;
        if ((rc = gen_expr(fc, e->as.index.index)) != VL_OK) return rc;
        return emit0(fc, OP_ARRGET);
    case EX_ARRAY:
        if (e->as.array.n < 0 || e->as.array.n > 0xFFFF) {
            set_cerr(fc->cs, "array literal too large");
            return VL_ERR_BAD_CODE;
        }
        for (i = 0; i < e->as.array.n; i++) {
            if ((rc = gen_expr(fc, e->as.array.items[i])) != VL_OK) return rc;
        }
        return emit_arg(fc, OP_NEWARRAY, e->as.array.n);
    case EX_MAP:
        if (e->as.map.n < 0 || e->as.map.n > 0xFFFF) {
            set_cerr(fc->cs, "map literal too large");
            return VL_ERR_BAD_CODE;
        }
        for (i = 0; i < e->as.map.n; i++) {
            if ((rc = gen_expr(fc, e->as.map.keys[i])) != VL_OK) return rc;
            if ((rc = gen_expr(fc, e->as.map.vals[i])) != VL_OK) return rc;
        }
        return emit_arg(fc, OP_NEWMAP, e->as.map.n);
    default:
        set_cerr(fc->cs, "unknown expression kind");
        return VL_ERR_BAD_CODE;
    }
}

/* Emit code for a `let`: evaluate the initializer, bind a new slot, store. */
static vl_status gen_let(fctx *fc, const vl_stmt *s) {
    vl_status rc;
    int slot;
    if ((rc = gen_expr(fc, s->as.let.init)) != VL_OK) return rc;
    slot = fc->next_slot++;
    if ((rc = scope_append(fc, s->as.let.name, slot)) != VL_OK) return rc;
    return emit_arg(fc, OP_STORELOCAL, slot);
}

/* Emit code for an assignment to a variable or an indexed element. */
static vl_status gen_assign(fctx *fc, const vl_stmt *s) {
    const vl_expr *target = s->as.assign.target;
    vl_status rc;

    if (target == NULL) {
        set_cerr(fc->cs, "invalid assignment target");
        return VL_ERR_BAD_CODE;
    }
    if (target->kind == EX_IDENT) {
        int slot;
        if ((rc = gen_expr(fc, s->as.assign.value)) != VL_OK) return rc;
        if (!scope_find(fc, target->as.ident.name, &slot)) {
            set_cerr_name(fc->cs, "assignment to unknown variable '%s'",
                          target->as.ident.name);
            return VL_ERR_BAD_CODE;
        }
        return emit_arg(fc, OP_STORELOCAL, slot);
    }
    if (target->kind == EX_INDEX) {
        if ((rc = gen_expr(fc, target->as.index.base)) != VL_OK) return rc;
        if ((rc = gen_expr(fc, target->as.index.index)) != VL_OK) return rc;
        if ((rc = gen_expr(fc, s->as.assign.value)) != VL_OK) return rc;
        return emit0(fc, OP_ARRSET);
    }
    set_cerr(fc->cs, "invalid assignment target");
    return VL_ERR_BAD_CODE;
}

/* Emit code for an `if`/`else` statement. */
static vl_status gen_if(fctx *fc, const vl_stmt *s) {
    vl_status rc;
    if ((rc = gen_expr(fc, s->as.if_s.cond)) != VL_OK) return rc;
    if (s->as.if_s.else_s != NULL) {
        int lelse, lend;
        if ((rc = new_label(fc, &lelse)) != VL_OK) return rc;
        if ((rc = new_label(fc, &lend)) != VL_OK) return rc;
        if ((rc = emit_jmp(fc, OP_JMPIFNOT, lelse)) != VL_OK) return rc;
        if ((rc = gen_stmt(fc, s->as.if_s.then_s)) != VL_OK) return rc;
        if ((rc = emit_jmp(fc, OP_JMP, lend)) != VL_OK) return rc;
        place_label(fc, lelse);
        if ((rc = gen_stmt(fc, s->as.if_s.else_s)) != VL_OK) return rc;
        place_label(fc, lend);
    } else {
        int lend;
        if ((rc = new_label(fc, &lend)) != VL_OK) return rc;
        if ((rc = emit_jmp(fc, OP_JMPIFNOT, lend)) != VL_OK) return rc;
        if ((rc = gen_stmt(fc, s->as.if_s.then_s)) != VL_OK) return rc;
        place_label(fc, lend);
    }
    return VL_OK;
}

/* Emit code for a `while` loop. */
static vl_status gen_while(fctx *fc, const vl_stmt *s) {
    int ltop, lend;
    vl_status rc;
    if ((rc = new_label(fc, &ltop)) != VL_OK) return rc;
    if ((rc = new_label(fc, &lend)) != VL_OK) return rc;
    place_label(fc, ltop);
    if ((rc = gen_expr(fc, s->as.while_s.cond)) != VL_OK) return rc;
    if ((rc = emit_jmp(fc, OP_JMPIFNOT, lend)) != VL_OK) return rc;
    if ((rc = gen_stmt(fc, s->as.while_s.body)) != VL_OK) return rc;
    if ((rc = emit_jmp(fc, OP_JMP, ltop)) != VL_OK) return rc;
    place_label(fc, lend);
    return VL_OK;
}

/* Emit code for a single statement. */
static vl_status gen_stmt(fctx *fc, const vl_stmt *s) {
    vl_status rc;
    int i;

    if (s == NULL) {
        return VL_OK;
    }

    switch (s->kind) {
    case ST_LET:
        return gen_let(fc, s);
    case ST_ASSIGN:
        return gen_assign(fc, s);
    case ST_IF:
        return gen_if(fc, s);
    case ST_WHILE:
        return gen_while(fc, s);
    case ST_RETURN:
        if (s->as.ret.value != NULL) {
            if ((rc = gen_expr(fc, s->as.ret.value)) != VL_OK) return rc;
        } else {
            if ((rc = emit0(fc, OP_PUSHNIL)) != VL_OK) return rc;
        }
        return emit0(fc, OP_RET);
    case ST_PRINT:
        if ((rc = gen_expr(fc, s->as.print.value)) != VL_OK) return rc;
        return emit0(fc, OP_PRINT);
    case ST_EXPR:
        if ((rc = gen_expr(fc, s->as.expr.e)) != VL_OK) return rc;
        return emit0(fc, OP_POP);
    case ST_BLOCK:
        for (i = 0; i < s->as.block.n; i++) {
            if ((rc = gen_stmt(fc, s->as.block.items[i])) != VL_OK) return rc;
        }
        return VL_OK;
    default:
        set_cerr(fc->cs, "unknown statement kind");
        return VL_ERR_BAD_CODE;
    }
}

/*
 * Assign each collected instruction its byte offset, resolve every jump's label
 * to a relative i16, and emit all instructions into the builder.
 */
static vl_status resolve_and_emit(fctx *fc) {
    cstate *cs = fc->cs;
    size_t i;
    uint32_t off = 0;
    vl_status rc = VL_OK;

    for (i = 0; i < fc->nins; i++) {
        fc->ins[i].offset = off;
        off += 1u + (uint32_t)fc->ins[i].size;
    }
    if (off > VL_MAX_CODE) {
        set_cerr(cs, "function too large");
        return VL_ERR_LIMIT;
    }

    for (i = 0; i < fc->nins; i++) {
        cins *in = &fc->ins[i];
        if (in->is_jump) {
            int idx = fc->labels[in->label];
            uint32_t target;
            int64_t rel;
            if (idx < 0) {
                set_cerr(cs, "internal: unplaced label");
                return VL_ERR_BAD_CODE;
            }
            target = (idx < (int)fc->nins) ? fc->ins[idx].offset : off;
            rel = (int64_t)target -
                  (int64_t)(in->offset + 1u + (uint32_t)in->size);
            if (rel < -32768 || rel > 32767) {
                set_cerr(cs, "jump out of range");
                return VL_ERR_BAD_CODE;
            }
            rc = vl_builder_emit_i16(cs->b, fc->func, in->op, (int16_t)rel);
        } else {
            switch (in->size) {
            case 0:
                rc = vl_builder_emit(cs->b, fc->func, in->op);
                break;
            case 1:
                rc = vl_builder_emit_u8(cs->b, fc->func, in->op,
                                        (uint8_t)in->imm);
                break;
            case 2:
                rc = vl_builder_emit_u16(cs->b, fc->func, in->op,
                                         (uint16_t)in->imm);
                break;
            case 4:
                rc = vl_builder_emit_i32(cs->b, fc->func, in->op,
                                         (int32_t)in->imm);
                break;
            default:
                rc = VL_ERR_BAD_CODE;
                break;
            }
        }
        if (rc != VL_OK) {
            set_cerr(cs, "emit failed");
            return rc;
        }
    }
    return VL_OK;
}

/* Release a function context's working buffers. */
static void fctx_free(fctx *fc) {
    vl_free(fc->ins);
    vl_free(fc->labels);
    vl_free(fc->scope);
    fc->ins = NULL;
    fc->labels = NULL;
    fc->scope = NULL;
}

/* Compile one source function into its (already begun) builder function. */
static vl_status gen_function(cstate *cs, int func_index) {
    const vl_func_def *fn = cs->p->funcs[func_index];
    fctx fc;
    vl_status rc;
    int j;

    memset(&fc, 0, sizeof(fc));
    fc.cs = cs;
    fc.func = func_index;
    fc.num_locals = fn->nparams + count_lets(fn->body);
    fc.next_slot = fn->nparams;

    for (j = 0; j < fn->nparams; j++) {
        rc = scope_append(&fc, fn->params[j], j);
        if (rc != VL_OK) {
            fctx_free(&fc);
            return rc;
        }
    }

    rc = gen_stmt(&fc, fn->body);
    if (rc != VL_OK) {
        fctx_free(&fc);
        return rc;
    }

    /* Guarantee control always terminates with a defined return value. */
    if ((rc = emit0(&fc, OP_PUSHNIL)) != VL_OK ||
        (rc = emit0(&fc, OP_RET)) != VL_OK) {
        fctx_free(&fc);
        return rc;
    }

    rc = resolve_and_emit(&fc);
    fctx_free(&fc);
    return rc;
}

/*
 * Compile a program to a freshly allocated .qbc buffer. Functions are begun in
 * program order (pass 1), the entry is set to "main", and each body is lowered
 * to bytecode (pass 2). On any error a short message is written to err and a
 * non-OK status returned; *out is only set on success.
 */
vl_status vl_compile(const vl_program *p, uint8_t **out, size_t *out_len,
                     char *err, size_t errcap) {
    cstate cs;
    int i;
    int main_index = -1;
    vl_status rc = VL_OK;

    cs.b = NULL;
    cs.p = p;
    cs.err = err;
    cs.errcap = errcap;

    if (out == NULL || out_len == NULL) {
        set_cerr(&cs, "invalid arguments");
        return VL_ERR_IO;
    }
    if (p == NULL || p->nfuncs <= 0) {
        set_cerr(&cs, "empty program");
        return VL_ERR_BAD_CODE;
    }

    cs.b = vl_builder_new();
    if (cs.b == NULL) {
        set_cerr(&cs, "out of memory");
        return VL_ERR_OOM;
    }

    /* Pass 1: begin every function so name->index is fixed before codegen. */
    for (i = 0; i < p->nfuncs; i++) {
        const vl_func_def *fn = p->funcs[i];
        int num_locals;
        int idx;
        if (fn->nparams < 0 || fn->nparams > 0xFF) {
            set_cerr(&cs, "too many parameters");
            rc = VL_ERR_LIMIT;
            goto done;
        }
        num_locals = fn->nparams + count_lets(fn->body);
        if (num_locals > VL_MAX_LOCALS) {
            set_cerr(&cs, "too many locals");
            rc = VL_ERR_LIMIT;
            goto done;
        }
        idx = vl_builder_begin_func(cs.b, (uint8_t)fn->nparams,
                                    (uint16_t)num_locals);
        if (idx < 0) {
            set_cerr(&cs, "out of memory");
            rc = VL_ERR_OOM;
            goto done;
        }
        if (strcmp(fn->name, "main") == 0 && main_index < 0) {
            if (fn->nparams != 0) {
                set_cerr(&cs, "main must take no parameters");
                rc = VL_ERR_BAD_CODE;
                goto done;
            }
            main_index = idx;
        }
    }

    if (main_index < 0) {
        set_cerr(&cs, "no main");
        rc = VL_ERR_BAD_CODE;
        goto done;
    }
    vl_builder_set_entry(cs.b, main_index);

    /* Pass 2: lower each function body to bytecode. */
    for (i = 0; i < p->nfuncs; i++) {
        rc = gen_function(&cs, i);
        if (rc != VL_OK) {
            goto done;
        }
    }

    rc = vl_builder_finish(cs.b, out, out_len);
    if (rc != VL_OK) {
        set_cerr(&cs, "module finalization failed");
    }

done:
    vl_builder_free(cs.b);
    return rc;
}

/*
 * Parse source text and compile it in one step. A parse error is reported
 * through err (by the parser) and propagated; otherwise the program is compiled
 * and freed before returning.
 */
vl_status vl_compile_source(const char *src, size_t len, uint8_t **out,
                            size_t *out_len, char *err, size_t errcap) {
    vl_program *prog = NULL;
    vl_status rc;

    rc = vl_parse(src, len, &prog, err, errcap);
    if (rc != VL_OK) {
        return rc;
    }
    rc = vl_compile(prog, out, out_len, err, errcap);
    vl_ast_free_program(prog);
    return rc;
}
