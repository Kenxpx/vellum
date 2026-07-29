/*
 * vm.c - the Vellum bytecode interpreter.
 *
 * Runs a verified module on a value stack with call frames, closures, and
 * upvalues over a reference-counted object heap. Every object reference held by
 * a stack slot, local, container, or upvalue owns a count; producers retain
 * before pushing and consumers release after popping. Execution is bounded by
 * an instruction budget and fixed stack/frame caps.
 */
#include "vm.h"

#include "intern.h"
#include "module.h"
#include "obj.h"
#include "opcodes.h"
#include "verify.h"

#define VL_OPERAND_MARGIN 256u /* operand-stack headroom reserved per frame */

typedef struct vl_frame {
    vl_closure *closure;
    const vl_function *fn;
    const uint8_t *ip;
    vl_value *base; /* locals[0] */
} vl_frame;

typedef struct vl_vm {
    const vl_module *m;
    vl_value *stack;
    uint32_t stack_cap;
    vl_value *sp; /* next free slot */
    vl_frame *frames;
    uint32_t frame_cap;
    uint32_t frame_count;
    vl_upvalue *open; /* intrusive list of open upvalues */
    vl_intern interns; /* interned string constants + their collector */
    uint64_t instr;
    vl_vm_limits limits;
} vl_vm;

void vl_vm_limits_default(vl_vm_limits *l) {
    l->instr_budget = 2000000;
    l->max_stack = 8192;
    l->max_frames = 256;
}

/* ------------------------------------------------------------ stack ops - */

static vl_status vm_push(vl_vm *vm, vl_value v) {
    if ((uint32_t)(vm->sp - vm->stack) >= vm->stack_cap) {
        return VL_ERR_STACK;
    }
    *vm->sp++ = v;
    return VL_OK;
}

static vl_value vm_pop(vl_vm *vm) { return *--vm->sp; }
static vl_value vm_peek(vl_vm *vm, uint32_t n) { return vm->sp[-1 - (int)n]; }

/* --------------------------------------------------------- upvalues ------ */

/* Find the shared open upvalue for a stack slot, or create and list one. */
static vl_upvalue *vm_capture(vl_vm *vm, vl_value *slot) {
    vl_upvalue *u = vm->open;
    while (u) {
        if (u->loc == slot) {
            return u;
        }
        u = u->next;
    }
    u = vl_upvalue_new(slot);
    if (!u) {
        return NULL;
    }
    u->next = vm->open;
    vm->open = u;
    return u;
}

/*
 * Close every open upvalue whose slot is at or above `from`, detaching it from
 * the stack so the closure keeps its own copy of the value.
 */
static void vm_close_upvals(vl_vm *vm, vl_value *from) {
    vl_upvalue **link = &vm->open;
    while (*link) {
        vl_upvalue *u = *link;
        if (u->loc >= from) {
            u->closed = *u->loc;
            u->loc = &u->closed;
            /* the closed upvalue takes ownership of the value it captured */
            vl_val_retain(u->closed);
            *link = u->next;
            u->next = NULL;
            vl_release((vl_obj *)u); /* release the open list's reference */
        } else {
            link = &u->next;
        }
    }
}

/* --------------------------------------------------------- frame setup --- */

static void vm_release_range(vl_value *lo, vl_value *hi) {
    vl_value *p;
    for (p = lo; p < hi; p++) {
        vl_val_release(*p);
    }
}

/*
 * Collect the intern table: mark every interned string reachable from a live
 * root, then sweep the rest. The roots are the values currently on the operand
 * stack (which spans every frame's locals and operands).
 */
#define VL_GC_MARK_DEPTH 8

/*
 * Mark every interned string reachable from a value, descending through arrays,
 * maps, and closures. Recursion is depth-bounded, which also keeps reference
 * cycles from looping forever.
 */
static void vm_mark_value(vl_value v, int depth) {
    vl_obj *o;
    if (v.tag != VL_OBJ || !v.as.o) {
        return;
    }
    o = v.as.o;
    if (o->otype == VL_OBJ_STRING) {
        vl_intern_mark((vl_string *)o);
        return;
    }
    if (depth <= 0) {
        return;
    }
    switch (o->otype) {
    case VL_OBJ_ARRAY: {
        vl_array *a = (vl_array *)o;
        uint32_t i;
        for (i = 0; i < a->len; i++) {
            vm_mark_value(a->items[i], depth - 1);
        }
        break;
    }
    case VL_OBJ_MAP: {
        vl_map *m = (vl_map *)o;
        uint32_t i;
        for (i = 0; i < m->cap; i++) {
            if (m->entries[i].used) {
                vm_mark_value(m->entries[i].val, depth - 1);
            }
        }
        break;
    }
    case VL_OBJ_CLOSURE: {
        vl_closure *c = (vl_closure *)o;
        uint8_t i;
        for (i = 0; i < c->num_upvals; i++) {
            vl_upvalue *u = c->upvals[i];
            if (u && u->loc != &u->closed) {
                vm_mark_value(*u->loc, depth - 1);
            }
        }
        break;
    }
    default:
        break;
    }
}

static void vm_intern_gc(vl_vm *vm) {
    vl_value *p;
    vl_intern_clear_marks(&vm->interns);
    for (p = vm->stack; p < vm->sp; p++) {
        vm_mark_value(*p, VL_GC_MARK_DEPTH);
    }
    vl_intern_sweep(&vm->interns);
}

/*
 * Push a constant. String constants are interned; a freshly interned string
 * that pushes the table past its collection threshold triggers a sweep.
 */
static vl_status vm_push_const(vl_vm *vm, const vl_const *k) {
    if (k->kind == QK_INT) {
        return vm_push(vm, vl_int(k->as.i));
    }
    if (k->kind == QK_REAL) {
        return vm_push(vm, vl_real(k->as.d));
    }
    {
        int is_new = 0;
        vl_string *s =
            vl_intern_get(&vm->interns, k->as.s.data, k->as.s.len, &is_new);
        vl_status st;
        if (!s) {
            return VL_ERR_OOM;
        }
        st = vm_push(vm, vl_obj_val((vl_obj *)s));
        if (st != VL_OK) {
            return st;
        }
        if (is_new && vl_intern_count(&vm->interns) >= VL_INTERN_GC_THRESHOLD) {
            vm_intern_gc(vm);
        }
        return VL_OK;
    }
}

/* -------------------------------------------------------- arithmetic ----- */

static vl_status vm_arith(vl_vm *vm, int op) {
    vl_value b = vm_pop(vm);
    vl_value a = vm_pop(vm);
    vl_value r;
    if (a.tag == VL_INT && b.tag == VL_INT) {
        int64_t x = a.as.i, y = b.as.i;
        switch (op) {
        case OP_ADD: r = vl_int(x + y); break;
        case OP_SUB: r = vl_int(x - y); break;
        case OP_MUL: r = vl_int(x * y); break;
        case OP_DIV:
            if (y == 0) { return VL_ERR_TRAP; }
            r = vl_int(x / y); break;
        case OP_MOD:
            if (y == 0) { return VL_ERR_TRAP; }
            r = vl_int(x % y); break;
        default: return VL_ERR_TYPE;
        }
    } else if ((a.tag == VL_INT || a.tag == VL_REAL) &&
               (b.tag == VL_INT || b.tag == VL_REAL)) {
        double x = a.tag == VL_INT ? (double)a.as.i : a.as.d;
        double y = b.tag == VL_INT ? (double)b.as.i : b.as.d;
        switch (op) {
        case OP_ADD: r = vl_real(x + y); break;
        case OP_SUB: r = vl_real(x - y); break;
        case OP_MUL: r = vl_real(x * y); break;
        case OP_DIV: r = vl_real(x / y); break;
        default: return VL_ERR_TYPE;
        }
    } else {
        vl_val_release(a);
        vl_val_release(b);
        return VL_ERR_TYPE;
    }
    vl_val_release(a);
    vl_val_release(b);
    return vm_push(vm, r);
}

static vl_status vm_compare(vl_vm *vm, int op) {
    vl_value b = vm_pop(vm);
    vl_value a = vm_pop(vm);
    int res = 0;
    if (op == OP_EQ) {
        res = vl_value_equal(a, b);
    } else if (op == OP_NE) {
        res = !vl_value_equal(a, b);
    } else if ((a.tag == VL_INT || a.tag == VL_REAL) &&
               (b.tag == VL_INT || b.tag == VL_REAL)) {
        double x = a.tag == VL_INT ? (double)a.as.i : a.as.d;
        double y = b.tag == VL_INT ? (double)b.as.i : b.as.d;
        switch (op) {
        case OP_LT: res = x < y; break;
        case OP_LE: res = x <= y; break;
        case OP_GT: res = x > y; break;
        case OP_GE: res = x >= y; break;
        default: break;
        }
    } else {
        vl_val_release(a);
        vl_val_release(b);
        return VL_ERR_TYPE;
    }
    vl_val_release(a);
    vl_val_release(b);
    return vm_push(vm, vl_bool(res));
}

/* --------------------------------------------------------- the loop ------ */

static vl_status vm_run(vl_vm *vm, vl_value *result) {
    vl_frame *fr = &vm->frames[vm->frame_count - 1];
    const uint8_t *ip = fr->ip;

#define SYNC() (fr->ip = ip)
#define RELOAD()                              \
    do {                                      \
        fr = &vm->frames[vm->frame_count - 1];\
        ip = fr->ip;                          \
    } while (0)

    for (;;) {
        uint8_t op;
        if (VL_UNLIKELY(vm->instr++ >= vm->limits.instr_budget)) {
            return VL_ERR_BUDGET;
        }
        op = *ip++;
        switch (op) {
        case OP_NOP:
            break;
        case OP_PUSHK: {
            uint16_t k = vl_load_u16le(ip); ip += 2;
            vl_status st = vm_push_const(vm, &vm->m->consts[k]);
            if (st != VL_OK) return st;
            break;
        }
        case OP_PUSHNIL: { vl_status st = vm_push(vm, vl_nil()); if (st) return st; break; }
        case OP_PUSHTRUE: { vl_status st = vm_push(vm, vl_bool(1)); if (st) return st; break; }
        case OP_PUSHFALSE: { vl_status st = vm_push(vm, vl_bool(0)); if (st) return st; break; }
        case OP_PUSHINT: {
            int32_t imm = (int32_t)vl_load_u32le(ip); ip += 4;
            vl_status st = vm_push(vm, vl_int(imm));
            if (st) return st;
            break;
        }
        case OP_POP: { vl_value v = vm_pop(vm); vl_val_release(v); break; }
        case OP_DUP: {
            vl_value v = vm_peek(vm, 0);
            vl_val_retain(v);
            { vl_status st = vm_push(vm, v); if (st) return st; }
            break;
        }
        case OP_SWAP: {
            vl_value t = vm->sp[-1];
            vm->sp[-1] = vm->sp[-2];
            vm->sp[-2] = t;
            break;
        }
        case OP_LOADLOCAL: {
            uint16_t slot = vl_load_u16le(ip); ip += 2;
            vl_value v = fr->base[slot];
            vl_val_retain(v);
            { vl_status st = vm_push(vm, v); if (st) return st; }
            break;
        }
        case OP_STORELOCAL: {
            uint16_t slot = vl_load_u16le(ip); ip += 2;
            vl_value v = vm_pop(vm);
            vl_val_release(fr->base[slot]);
            fr->base[slot] = v;
            break;
        }
        case OP_GETUPVAL: {
            uint8_t idx = *ip++;
            vl_upvalue *u = fr->closure->upvals[idx];
            vl_value v = *u->loc;
            vl_val_retain(v);
            { vl_status st = vm_push(vm, v); if (st) return st; }
            break;
        }
        case OP_SETUPVAL: {
            uint8_t idx = *ip++;
            vl_upvalue *u = fr->closure->upvals[idx];
            vl_value v = vm_pop(vm);
            vl_val_release(*u->loc);
            *u->loc = v;
            break;
        }
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: {
            vl_status st = vm_arith(vm, op); if (st) return st; break;
        }
        case OP_NEG: {
            vl_value a = vm_pop(vm);
            if (a.tag == VL_INT) { vl_status st = vm_push(vm, vl_int(-a.as.i)); if (st) return st; }
            else if (a.tag == VL_REAL) { vl_status st = vm_push(vm, vl_real(-a.as.d)); if (st) return st; }
            else { vl_val_release(a); return VL_ERR_TYPE; }
            break;
        }
        case OP_NOT: {
            vl_value a = vm_pop(vm);
            int t = vl_value_truthy(a);
            vl_val_release(a);
            { vl_status st = vm_push(vm, vl_bool(!t)); if (st) return st; }
            break;
        }
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            vl_status st = vm_compare(vm, op); if (st) return st; break;
        }
        case OP_JMP: {
            int16_t rel = (int16_t)vl_load_u16le(ip); ip += 2;
            ip += rel;
            break;
        }
        case OP_JMPIF: {
            int16_t rel = (int16_t)vl_load_u16le(ip); ip += 2;
            vl_value c = vm_pop(vm);
            int t = vl_value_truthy(c);
            vl_val_release(c);
            if (t) ip += rel;
            break;
        }
        case OP_JMPIFNOT: {
            int16_t rel = (int16_t)vl_load_u16le(ip); ip += 2;
            vl_value c = vm_pop(vm);
            int t = vl_value_truthy(c);
            vl_val_release(c);
            if (!t) ip += rel;
            break;
        }
        case OP_NEWARRAY: {
            uint16_t n = vl_load_u16le(ip); ip += 2;
            vl_array *a = vl_array_new();
            uint16_t i;
            if (!a) return VL_ERR_OOM;
            /* values are on the stack in order; move them in */
            for (i = 0; i < n; i++) {
                vl_value v = vm->sp[-(int)n + i];
                vl_status st = vl_array_push(a, v);
                if (st) { vl_release((vl_obj *)a); return st; }
            }
            for (i = 0; i < n; i++) { vl_val_release(vm_pop(vm)); }
            { vl_status st = vm_push(vm, vl_obj_val((vl_obj *)a)); if (st) return st; }
            break;
        }
        case OP_ARRGET: {
            vl_value idx = vm_pop(vm);
            vl_value av = vm_pop(vm);
            vl_value out;
            vl_status st;
            if (av.tag != VL_OBJ || av.as.o->otype != VL_OBJ_ARRAY || idx.tag != VL_INT) {
                vl_val_release(idx); vl_val_release(av); return VL_ERR_TYPE;
            }
            st = vl_array_get((vl_array *)av.as.o, idx.as.i, &out);
            if (st) { vl_val_release(av); return st; }
            vl_val_retain(out);
            vl_val_release(av);
            { vl_status s2 = vm_push(vm, out); if (s2) return s2; }
            break;
        }
        case OP_ARRSET: {
            vl_value v = vm_pop(vm);
            vl_value idx = vm_pop(vm);
            vl_value av = vm_pop(vm);
            vl_status st;
            if (av.tag != VL_OBJ || av.as.o->otype != VL_OBJ_ARRAY || idx.tag != VL_INT) {
                vl_val_release(v); vl_val_release(idx); vl_val_release(av); return VL_ERR_TYPE;
            }
            st = vl_array_set((vl_array *)av.as.o, idx.as.i, v);
            vl_val_release(v);
            vl_val_release(av);
            if (st) return st;
            break;
        }
        case OP_ARRPUSH: {
            vl_value v = vm_pop(vm);
            vl_value av = vm_pop(vm);
            vl_status st;
            if (av.tag != VL_OBJ || av.as.o->otype != VL_OBJ_ARRAY) {
                vl_val_release(v); vl_val_release(av); return VL_ERR_TYPE;
            }
            st = vl_array_push((vl_array *)av.as.o, v);
            vl_val_release(v);
            vl_val_release(av);
            if (st) return st;
            break;
        }
        case OP_LEN: {
            vl_value c = vm_pop(vm);
            int64_t n = 0;
            if (c.tag == VL_OBJ && c.as.o->otype == VL_OBJ_ARRAY) n = ((vl_array *)c.as.o)->len;
            else if (c.tag == VL_OBJ && c.as.o->otype == VL_OBJ_MAP) n = ((vl_map *)c.as.o)->count;
            else if (c.tag == VL_OBJ && c.as.o->otype == VL_OBJ_STRING) n = ((vl_string *)c.as.o)->len;
            else { vl_val_release(c); return VL_ERR_TYPE; }
            vl_val_release(c);
            { vl_status st = vm_push(vm, vl_int(n)); if (st) return st; }
            break;
        }
        case OP_NEWMAP: {
            uint16_t n = vl_load_u16le(ip); ip += 2;
            vl_map *mp = vl_map_new();
            uint16_t i;
            if (!mp) return VL_ERR_OOM;
            for (i = 0; i < n; i++) {
                vl_value key = vm->sp[-2 * (int)n + 2 * i];
                vl_value val = vm->sp[-2 * (int)n + 2 * i + 1];
                vl_status st = vl_map_set(mp, key, val);
                if (st) { vl_release((vl_obj *)mp); return st; }
            }
            for (i = 0; i < 2 * n; i++) { vl_val_release(vm_pop(vm)); }
            { vl_status st = vm_push(vm, vl_obj_val((vl_obj *)mp)); if (st) return st; }
            break;
        }
        case OP_MAPGET: {
            vl_value key = vm_pop(vm);
            vl_value mv = vm_pop(vm);
            vl_map *mp;
            vl_value out;
            if (mv.tag != VL_OBJ || mv.as.o->otype != VL_OBJ_MAP) {
                vl_val_release(key); vl_val_release(mv); return VL_ERR_TYPE;
            }
            mp = (vl_map *)mv.as.o;
            if (vl_map_get(mp, key, &out)) {
                /*
                 * Fast typed read: a map that has only ever held object values
                 * (per its recorded value kind) yields an object reference, so
                 * retain it directly without re-checking the tag.
                 */
                vl_val_retain(out);
            } else {
                out = vl_nil();
            }
            vl_val_release(key);
            vl_val_release(mv);
            { vl_status st = vm_push(vm, out); if (st) return st; }
            break;
        }
        case OP_MAPSET: {
            vl_value v = vm_pop(vm);
            vl_value key = vm_pop(vm);
            vl_value mv = vm_pop(vm);
            vl_status st;
            if (mv.tag != VL_OBJ || mv.as.o->otype != VL_OBJ_MAP) {
                vl_val_release(v); vl_val_release(key); vl_val_release(mv); return VL_ERR_TYPE;
            }
            st = vl_map_set((vl_map *)mv.as.o, key, v);
            vl_val_release(v);
            vl_val_release(key);
            vl_val_release(mv);
            if (st) return st;
            break;
        }
        case OP_CLOSURE: {
            uint16_t fidx = vl_load_u16le(ip); ip += 2;
            const vl_function *proto = &vm->m->funcs[fidx];
            vl_closure *c = vl_closure_new(fidx, proto->num_upvals);
            uint8_t i;
            if (!c) return VL_ERR_OOM;
            for (i = 0; i < proto->num_upvals; i++) {
                vl_upval_desc d = proto->upvals[i];
                vl_upvalue *u;
                if (d.is_local) {
                    if (d.index >= fr->fn->num_locals) {
                        vl_release((vl_obj *)c);
                        return VL_ERR_BAD_CODE;
                    }
                    u = vm_capture(vm, fr->base + d.index);
                    if (!u) { vl_release((vl_obj *)c); return VL_ERR_OOM; }
                } else {
                    if (d.index >= fr->closure->num_upvals) {
                        vl_release((vl_obj *)c);
                        return VL_ERR_BAD_CODE;
                    }
                    u = fr->closure->upvals[d.index];
                }
                c->upvals[i] = u;
                vl_retain((vl_obj *)u);
            }
            { vl_status st = vm_push(vm, vl_obj_val((vl_obj *)c)); if (st) { return st; } }
            break;
        }
        case OP_CALL: {
            uint8_t argc = *ip++;
            vl_value cv = vm_peek(vm, argc);
            vl_closure *c;
            const vl_function *callee;
            uint32_t base_idx;
            uint16_t i;
            if (cv.tag != VL_OBJ || cv.as.o->otype != VL_OBJ_CLOSURE) return VL_ERR_TYPE;
            c = (vl_closure *)cv.as.o;
            callee = &vm->m->funcs[c->func];
            if (argc != callee->arity) return VL_ERR_TYPE;
            if (vm->frame_count >= vm->frame_cap) return VL_ERR_LIMIT;
            base_idx = (uint32_t)((vm->sp - argc) - vm->stack);
            /* correct room check: the callee's locals plus operand headroom */
            if (base_idx + callee->num_locals + VL_OPERAND_MARGIN > vm->stack_cap) {
                return VL_ERR_STACK;
            }
            /* nil the locals beyond the arguments */
            for (i = argc; i < callee->num_locals; i++) {
                vm->stack[base_idx + i] = vl_nil();
            }
            vm->sp = vm->stack + base_idx + callee->num_locals;
            SYNC();
            {
                vl_frame *nf = &vm->frames[vm->frame_count++];
                nf->closure = c;
                nf->fn = callee;
                nf->ip = callee->code;
                nf->base = vm->stack + base_idx;
            }
            RELOAD();
            break;
        }
        case OP_TAILCALL: {
            uint8_t argc = *ip++;
            vl_value cv = vm_peek(vm, argc);
            vl_closure *c;
            const vl_function *callee;
            vl_value tmp[64];
            vl_value *base = fr->base;
            uint32_t base_idx;
            uint16_t i;
            if (cv.tag != VL_OBJ || cv.as.o->otype != VL_OBJ_CLOSURE) return VL_ERR_TYPE;
            c = (vl_closure *)cv.as.o;
            callee = &vm->m->funcs[c->func];
            if (argc != callee->arity) return VL_ERR_TYPE;
            base_idx = (uint32_t)(base - vm->stack);
            /* room check for the reused frame */
            if (base_idx + callee->num_locals + VL_OPERAND_MARGIN > vm->stack_cap) {
                return VL_ERR_STACK;
            }
            /* take ownership of the arguments and the new closure */
            for (i = 0; i < argc; i++) {
                tmp[i] = vm->sp[-(int)argc + i];
            }
            vm->sp -= argc;         /* args (ownership moved to tmp)       */
            vl_retain(cv.as.o);     /* keep the new closure across teardown */
            (void)vm_pop(vm);       /* pop the closure slot                 */
            /* detach any upvalues that captured this frame before it is reused */
            vm_close_upvals(vm, base);
            /* release the old frame's remaining slots and old closure */
            vm_release_range(base - 1, vm->sp);
            /* install the new closure and arguments */
            vm->stack[base_idx - 1] = cv;
            for (i = 0; i < argc; i++) {
                base[i] = tmp[i];
            }
            for (i = argc; i < callee->num_locals; i++) {
                base[i] = vl_nil();
            }
            vm->sp = base + callee->num_locals;
            fr->closure = c;
            fr->fn = callee;
            fr->ip = callee->code;
            ip = callee->code;
            break;
        }
        case OP_RET: {
            vl_value ret = vm_pop(vm);
            vl_value *base = fr->base;
            vm_close_upvals(vm, base);
            vm_release_range(base - 1, vm->sp);
            vm->sp = base - 1;
            vm->frame_count--;
            if (vm->frame_count == 0) {
                if (result) { *result = ret; }
                else { vl_val_release(ret); }
                return VL_OK;
            }
            vm->stack[base - 1 - vm->stack] = ret;
            vm->sp = base; /* result occupies base-1; top is base */
            RELOAD();
            break;
        }
        case OP_CLOSEUPVALS: {
            uint16_t slot = vl_load_u16le(ip); ip += 2;
            vm_close_upvals(vm, fr->base + slot);
            break;
        }
        case OP_PRINT: {
            vl_value v = vm_pop(vm);
            vl_val_release(v);
            break;
        }
        case OP_HALT: {
            if (result) { *result = vl_nil(); }
            return VL_OK;
        }
        default:
            return VL_ERR_BAD_CODE;
        }
    }
#undef SYNC
#undef RELOAD
}

/* Tear down any state left after an error, releasing all live references. */
static void vm_unwind(vl_vm *vm) {
    vl_upvalue *u = vm->open;
    while (u) {
        vl_upvalue *n = u->next;
        u->next = NULL;
        u = n;
    }
    vm->open = NULL;
    if (vm->frame_count > 0) {
        vl_value *bottom = vm->frames[0].base - 1;
        vm_release_range(bottom, vm->sp);
    }
}

vl_status vl_run_module(const vl_module *m, const vl_vm_limits *limits,
                        vl_value *result) {
    vl_vm vm;
    vl_vm_limits lim;
    const vl_function *entry;
    vl_closure *ec;
    vl_status st;
    uint16_t i;

    if (result) { *result = vl_nil(); }
    vl_vm_limits_default(&lim);
    if (limits) { lim = *limits; }
    if (lim.max_stack > VL_MAX_STACK) { lim.max_stack = VL_MAX_STACK; }
    if (lim.max_frames > VL_MAX_FRAMES) { lim.max_frames = VL_MAX_FRAMES; }

    if (m->entry >= m->func_count) { return VL_ERR_BAD_MODULE; }
    entry = &m->funcs[m->entry];
    if (entry->arity != 0 || entry->num_upvals != 0) { return VL_ERR_BAD_MODULE; }

    memset(&vm, 0, sizeof(vm));
    vm.m = m;
    vm.limits = lim;
    vm.stack_cap = lim.max_stack;
    vm.stack = (vl_value *)vl_malloc((size_t)vm.stack_cap * sizeof(vl_value));
    vm.frames = (vl_frame *)vl_malloc((size_t)lim.max_frames * sizeof(vl_frame));
    if (!vm.stack || !vm.frames) {
        vl_free(vm.stack);
        vl_free(vm.frames);
        return VL_ERR_OOM;
    }
    vm.frame_cap = lim.max_frames;

    ec = vl_closure_new(m->entry, 0);
    if (!ec) { vl_free(vm.stack); vl_free(vm.frames); return VL_ERR_OOM; }

    /* stack[0] holds the entry closure; the entry frame's locals start at 1 */
    vm.stack[0] = vl_obj_val((vl_obj *)ec);
    vm.sp = vm.stack + 1;
    if (1u + entry->num_locals + VL_OPERAND_MARGIN > vm.stack_cap) {
        vl_release((vl_obj *)ec);
        vl_free(vm.stack); vl_free(vm.frames);
        return VL_ERR_STACK;
    }
    for (i = 0; i < entry->num_locals; i++) { vm.stack[1 + i] = vl_nil(); }
    vm.sp = vm.stack + 1 + entry->num_locals;
    vm.frames[0].closure = ec;
    vm.frames[0].fn = entry;
    vm.frames[0].ip = entry->code;
    vm.frames[0].base = vm.stack + 1;
    vm.frame_count = 1;

    st = vm_run(&vm, result);
    if (st != VL_OK) {
        vm_unwind(&vm);
    }
    /* A string result is interned; hand the caller an independent copy before
     * the intern table (which owns interned strings) is torn down. */
    if (st == VL_OK && result && result->tag == VL_OBJ && result->as.o &&
        result->as.o->otype == VL_OBJ_STRING &&
        ((vl_string *)result->as.o)->interned) {
        vl_string *src = (vl_string *)result->as.o;
        vl_string *copy = vl_string_new(src->data, src->len);
        *result = copy ? vl_obj_val((vl_obj *)copy) : vl_nil();
    }
    vl_intern_dispose(&vm.interns);
    vl_free(vm.stack);
    vl_free(vm.frames);
    return st;
}

vl_status vl_exec(const uint8_t *data, size_t size, const vl_vm_limits *limits) {
    vl_module *m = NULL;
    vl_value result;
    vl_status st = vl_module_load(data, size, &m);
    if (st != VL_OK) { return st; }
    st = vl_verify_module(m);
    if (st != VL_OK) { vl_module_free(m); return st; }
    st = vl_run_module(m, limits, &result);
    if (st == VL_OK) { vl_val_release(result); }
    vl_module_free(m);
    return st;
}
