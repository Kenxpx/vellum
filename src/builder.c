/*
 * builder.c - a programmatic builder that assembles a Vellum module in memory
 * and serializes it to the .qbc byte layout the loader reads (see
 * docs/BYTECODE.md). Constants and code accumulate in growable, overflow-checked
 * buffers; vl_builder_finish emits a single fresh, self-contained buffer.
 */
#include "builder.h"

#include "module.h"
#include "varint.h"

/* One function under construction: header fields plus growable upvals + code. */
typedef struct bfunc {
    uint8_t arity;
    uint16_t num_locals;
    vl_upval_desc *upvals;
    size_t upval_count;
    size_t upval_cap;
    uint8_t *code;
    size_t code_len;
    size_t code_cap;
} bfunc;

struct vl_builder {
    vl_const *consts; /* reuses vl_const; strings are owned copies */
    size_t const_count;
    size_t const_cap;
    bfunc *funcs;
    size_t func_count;
    size_t func_cap;
    uint32_t entry;
};

/*
 * Grow an element array to at least one more slot, doubling capacity with
 * overflow-checked arithmetic. Returns the (re)allocated pointer and the new
 * capacity in *newcap, or NULL on overflow/allocation failure (arr untouched).
 */
static void *grow_array(void *arr, size_t cap, size_t elem, size_t *newcap) {
    size_t nc = cap ? cap : 8;
    size_t bytes;
    void *p;
    if (cap && !vl_size_mul(cap, 2, &nc)) {
        return NULL;
    }
    if (!vl_size_mul(nc, elem, &bytes)) {
        return NULL;
    }
    p = vl_realloc(arr, bytes);
    if (p == NULL) {
        return NULL;
    }
    *newcap = nc;
    return p;
}

/* Ensure *buf holds at least need bytes, doubling capacity as required. */
static vl_status ensure_bytes(uint8_t **buf, size_t *cap, size_t need) {
    size_t nc;
    uint8_t *p;
    if (*cap >= need) {
        return VL_OK;
    }
    nc = *cap ? *cap : 8;
    while (nc < need) {
        if (!vl_size_mul(nc, 2, &nc)) {
            return VL_ERR_OVERFLOW;
        }
    }
    p = vl_realloc(*buf, nc);
    if (p == NULL) {
        return VL_ERR_OOM;
    }
    *buf = p;
    *cap = nc;
    return VL_OK;
}

/* Reserve room for one more constant. Returns 1 on success, 0 on failure. */
static int reserve_const(vl_builder *b) {
    size_t nc;
    void *p;
    if (b->const_count < b->const_cap) {
        return 1;
    }
    p = grow_array(b->consts, b->const_cap, sizeof(vl_const), &nc);
    if (p == NULL) {
        return 0;
    }
    b->consts = p;
    b->const_cap = nc;
    return 1;
}

/* Reserve room for one more function. Returns 1 on success, 0 on failure. */
static int reserve_func(vl_builder *b) {
    size_t nc;
    void *p;
    if (b->func_count < b->func_cap) {
        return 1;
    }
    p = grow_array(b->funcs, b->func_cap, sizeof(bfunc), &nc);
    if (p == NULL) {
        return 0;
    }
    b->funcs = p;
    b->func_cap = nc;
    return 1;
}

/* Resolve a function index to its builder record, bounds-checked. */
static vl_status get_func(vl_builder *b, int func, bfunc **out) {
    if (b == NULL || func < 0 || (size_t)func >= b->func_count) {
        return VL_ERR_LIMIT;
    }
    *out = &b->funcs[func];
    return VL_OK;
}

/* Append n bytes to a function's code buffer, growing it as needed. */
static vl_status code_append(bfunc *f, const uint8_t *src, size_t n) {
    size_t need;
    vl_status st;
    if (!vl_size_add(f->code_len, n, &need)) {
        return VL_ERR_OVERFLOW;
    }
    st = ensure_bytes(&f->code, &f->code_cap, need);
    if (st != VL_OK) {
        return st;
    }
    memcpy(f->code + f->code_len, src, n);
    f->code_len = need;
    return VL_OK;
}

/* Allocate an empty builder, or NULL on allocation failure. */
vl_builder *vl_builder_new(void) {
    return vl_calloc(1, sizeof(vl_builder));
}

/* Release a builder and every buffer it owns. NULL-safe. */
void vl_builder_free(vl_builder *b) {
    size_t i;
    if (b == NULL) {
        return;
    }
    if (b->consts != NULL) {
        for (i = 0; i < b->const_count; i++) {
            if (b->consts[i].kind == QK_STRING) {
                vl_free(b->consts[i].as.s.data);
            }
        }
        vl_free(b->consts);
    }
    if (b->funcs != NULL) {
        for (i = 0; i < b->func_count; i++) {
            vl_free(b->funcs[i].upvals);
            vl_free(b->funcs[i].code);
        }
        vl_free(b->funcs);
    }
    vl_free(b);
}

/* Append an integer constant; returns its pool index or -1 on failure. */
int vl_builder_const_int(vl_builder *b, int64_t v) {
    vl_const *k;
    if (b == NULL || !reserve_const(b)) {
        return -1;
    }
    k = &b->consts[b->const_count];
    k->kind = QK_INT;
    k->as.i = v;
    return (int)b->const_count++;
}

/* Append a real constant; returns its pool index or -1 on failure. */
int vl_builder_const_real(vl_builder *b, double v) {
    vl_const *k;
    if (b == NULL || !reserve_const(b)) {
        return -1;
    }
    k = &b->consts[b->const_count];
    k->kind = QK_REAL;
    k->as.d = v;
    return (int)b->const_count++;
}

/* Append a string constant (bytes copied); returns its index or -1 on failure. */
int vl_builder_const_string(vl_builder *b, const char *s, uint32_t len) {
    vl_const *k;
    char *copy;
    size_t total;
    if (b == NULL) {
        return -1;
    }
    if (!vl_size_add(len, 1, &total)) {
        return -1;
    }
    copy = vl_malloc(total);
    if (copy == NULL) {
        return -1;
    }
    if (len > 0) {
        memcpy(copy, s, len);
    }
    copy[len] = '\0';
    if (!reserve_const(b)) {
        vl_free(copy);
        return -1;
    }
    k = &b->consts[b->const_count];
    k->kind = QK_STRING;
    k->as.s.data = copy;
    k->as.s.len = len;
    return (int)b->const_count++;
}

/* Begin a function; returns its index or -1 on allocation failure. */
int vl_builder_begin_func(vl_builder *b, uint8_t arity, uint16_t num_locals) {
    bfunc *f;
    if (b == NULL || !reserve_func(b)) {
        return -1;
    }
    f = &b->funcs[b->func_count];
    memset(f, 0, sizeof(*f));
    f->arity = arity;
    f->num_locals = num_locals;
    return (int)b->func_count++;
}

/* Append an upvalue capture descriptor to a function. */
vl_status vl_builder_add_upval(vl_builder *b, int func, uint8_t is_local,
                               uint8_t index) {
    bfunc *f;
    vl_status st = get_func(b, func, &f);
    if (st != VL_OK) {
        return st;
    }
    if (f->upval_count >= VL_MAX_UPVALS) {
        return VL_ERR_LIMIT;
    }
    if (f->upval_count == f->upval_cap) {
        size_t nc;
        void *p = grow_array(f->upvals, f->upval_cap, sizeof(vl_upval_desc), &nc);
        if (p == NULL) {
            return VL_ERR_OOM;
        }
        f->upvals = p;
        f->upval_cap = nc;
    }
    f->upvals[f->upval_count].is_local = is_local;
    f->upvals[f->upval_count].index = index;
    f->upval_count++;
    return VL_OK;
}

/* Set the module entry function index. */
void vl_builder_set_entry(vl_builder *b, int func) {
    if (b == NULL || func < 0) {
        return;
    }
    b->entry = (uint32_t)func;
}

/* Emit a bare opcode into a function. */
vl_status vl_builder_emit(vl_builder *b, int func, uint8_t op) {
    bfunc *f;
    vl_status st = get_func(b, func, &f);
    if (st != VL_OK) {
        return st;
    }
    return code_append(f, &op, 1);
}

/* Emit an opcode followed by a u8 operand. */
vl_status vl_builder_emit_u8(vl_builder *b, int func, uint8_t op, uint8_t a) {
    bfunc *f;
    uint8_t bytes[2];
    vl_status st = get_func(b, func, &f);
    if (st != VL_OK) {
        return st;
    }
    bytes[0] = op;
    bytes[1] = a;
    return code_append(f, bytes, sizeof(bytes));
}

/* Emit an opcode followed by a little-endian u16 operand. */
vl_status vl_builder_emit_u16(vl_builder *b, int func, uint8_t op, uint16_t a) {
    bfunc *f;
    uint8_t bytes[3];
    vl_status st = get_func(b, func, &f);
    if (st != VL_OK) {
        return st;
    }
    bytes[0] = op;
    vl_store_u16le(bytes + 1, a);
    return code_append(f, bytes, sizeof(bytes));
}

/* Emit an opcode followed by a little-endian i16 operand. */
vl_status vl_builder_emit_i16(vl_builder *b, int func, uint8_t op, int16_t a) {
    bfunc *f;
    uint8_t bytes[3];
    vl_status st = get_func(b, func, &f);
    if (st != VL_OK) {
        return st;
    }
    bytes[0] = op;
    vl_store_u16le(bytes + 1, (uint16_t)a);
    return code_append(f, bytes, sizeof(bytes));
}

/* Emit an opcode followed by a little-endian i32 operand. */
vl_status vl_builder_emit_i32(vl_builder *b, int func, uint8_t op, int32_t a) {
    bfunc *f;
    uint8_t bytes[5];
    vl_status st = get_func(b, func, &f);
    if (st != VL_OK) {
        return st;
    }
    bytes[0] = op;
    vl_store_u32le(bytes + 1, (uint32_t)a);
    return code_append(f, bytes, sizeof(bytes));
}

/*
 * Serialize the builder to a freshly allocated .qbc buffer (*out, *out_len),
 * laid out exactly as docs/BYTECODE.md describes. Sizes are computed with
 * overflow-checked arithmetic before a single allocation. Caller frees *out.
 */
vl_status vl_builder_finish(vl_builder *b, uint8_t **out, size_t *out_len) {
    size_t total = 20; /* header */
    uint8_t *buf;
    size_t pos;
    size_t i;

    if (b == NULL || out == NULL || out_len == NULL) {
        return VL_ERR_BAD_MODULE;
    }
    *out = NULL;
    *out_len = 0;

    /* Size the constant pool. */
    for (i = 0; i < b->const_count; i++) {
        vl_const *k = &b->consts[i];
        if (!vl_size_add(total, 1, &total)) {
            return VL_ERR_OVERFLOW;
        }
        switch (k->kind) {
        case QK_INT:
        case QK_REAL:
            if (!vl_size_add(total, 8, &total)) {
                return VL_ERR_OVERFLOW;
            }
            break;
        case QK_STRING:
            if (!vl_size_add(total, vl_varint_size(k->as.s.len), &total)) {
                return VL_ERR_OVERFLOW;
            }
            if (!vl_size_add(total, k->as.s.len, &total)) {
                return VL_ERR_OVERFLOW;
            }
            break;
        default:
            return VL_ERR_BAD_CONST;
        }
    }

    /* Size the function table. */
    for (i = 0; i < b->func_count; i++) {
        bfunc *f = &b->funcs[i];
        size_t upbytes;
        /* arity(1) + num_locals(2) + num_upvals(1) */
        if (!vl_size_add(total, 4, &total)) {
            return VL_ERR_OVERFLOW;
        }
        if (!vl_size_mul(f->upval_count, 2, &upbytes)) {
            return VL_ERR_OVERFLOW;
        }
        if (!vl_size_add(total, upbytes, &total)) {
            return VL_ERR_OVERFLOW;
        }
        /* code_len(4) + code bytes */
        if (!vl_size_add(total, 4, &total)) {
            return VL_ERR_OVERFLOW;
        }
        if (!vl_size_add(total, f->code_len, &total)) {
            return VL_ERR_OVERFLOW;
        }
    }

    buf = vl_malloc(total);
    if (buf == NULL) {
        return VL_ERR_OOM;
    }

    /* Header (20 bytes). */
    vl_store_u32le(buf, VL_MAGIC);
    vl_store_u16le(buf + 4, VL_VERSION);
    vl_store_u16le(buf + 6, 0);
    vl_store_u32le(buf + 8, (uint32_t)b->const_count);
    vl_store_u32le(buf + 12, (uint32_t)b->func_count);
    vl_store_u32le(buf + 16, b->entry);
    pos = 20;

    /* Constant pool. */
    for (i = 0; i < b->const_count; i++) {
        vl_const *k = &b->consts[i];
        buf[pos++] = k->kind;
        switch (k->kind) {
        case QK_INT:
            vl_store_u64le(buf + pos, (uint64_t)k->as.i);
            pos += 8;
            break;
        case QK_REAL: {
            uint64_t bits;
            memcpy(&bits, &k->as.d, sizeof(bits));
            vl_store_u64le(buf + pos, bits);
            pos += 8;
            break;
        }
        case QK_STRING:
            pos += vl_varint_encode(k->as.s.len, buf + pos, total - pos);
            if (k->as.s.len > 0) {
                memcpy(buf + pos, k->as.s.data, k->as.s.len);
                pos += k->as.s.len;
            }
            break;
        default:
            vl_free(buf);
            return VL_ERR_BAD_CONST;
        }
    }

    /* Function table. */
    for (i = 0; i < b->func_count; i++) {
        bfunc *f = &b->funcs[i];
        size_t j;
        buf[pos++] = f->arity;
        vl_store_u16le(buf + pos, f->num_locals);
        pos += 2;
        buf[pos++] = (uint8_t)f->upval_count;
        for (j = 0; j < f->upval_count; j++) {
            buf[pos++] = f->upvals[j].is_local;
            buf[pos++] = f->upvals[j].index;
        }
        vl_store_u32le(buf + pos, (uint32_t)f->code_len);
        pos += 4;
        if (f->code_len > 0) {
            memcpy(buf + pos, f->code, f->code_len);
            pos += f->code_len;
        }
    }

    *out = buf;
    *out_len = total;
    return VL_OK;
}
