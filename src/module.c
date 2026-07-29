/*
 * module.c - load and structurally validate a Vellum module (.qbc).
 *
 * Everything is parsed through a bounded vl_cursor over the caller's buffer;
 * all string bytes and bytecode are copied into module-owned allocations so the
 * loaded module never retains a pointer into the input. On any failure the
 * partially built module is freed and the failing status is returned.
 */
#include "module.h"

#include "cursor.h"
#include "internal.h"

/*
 * Read one constant-pool entry into *k (which must be zeroed by the caller so
 * cleanup is safe on partial failure). Returns a vl_status.
 */
static vl_status load_const(vl_cursor *c, vl_const *k) {
    uint8_t kind;
    vl_status st = vl_read_u8(c, &kind);
    if (st != VL_OK) {
        return st;
    }
    k->kind = kind;
    switch (kind) {
    case QK_INT: {
        int64_t v;
        st = vl_read_i64(c, &v);
        if (st != VL_OK) {
            return st;
        }
        k->as.i = v;
        return VL_OK;
    }
    case QK_REAL: {
        double v;
        st = vl_read_f64(c, &v);
        if (st != VL_OK) {
            return st;
        }
        k->as.d = v;
        return VL_OK;
    }
    case QK_STRING: {
        uint64_t len;
        char *buf;
        size_t n;
        st = vl_read_varint(c, &len);
        if (st != VL_OK) {
            return st;
        }
        if (len > VL_MAX_STRING) {
            return VL_ERR_BAD_CONST;
        }
        n = (size_t)len;
        buf = vl_malloc(n);
        if (buf == NULL) {
            return VL_ERR_OOM;
        }
        st = vl_read_bytes(c, buf, n);
        if (st != VL_OK) {
            vl_free(buf);
            return st;
        }
        buf[n] = '\0';
        k->as.s.data = buf;
        k->as.s.len = (uint32_t)n;
        return VL_OK;
    }
    default:
        return VL_ERR_BAD_CONST;
    }
}

/*
 * Read one function-table entry into *f (which must be zeroed by the caller so
 * cleanup is safe on partial failure). Returns a vl_status.
 */
static vl_status load_func(vl_cursor *c, vl_function *f) {
    uint8_t arity;
    uint16_t num_locals;
    uint8_t num_upvals;
    uint32_t code_len;
    vl_status st;

    if ((st = vl_read_u8(c, &arity)) != VL_OK) {
        return st;
    }
    if ((st = vl_read_u16(c, &num_locals)) != VL_OK) {
        return st;
    }
    if ((st = vl_read_u8(c, &num_upvals)) != VL_OK) {
        return st;
    }
    /*
     * num_locals (u16) and num_upvals (u8) cannot exceed VL_MAX_LOCALS (0xFFFF)
     * or VL_MAX_UPVALS (0xFF) by construction; only num_locals >= arity needs
     * checking.
     */
    if (num_locals < arity) {
        return VL_ERR_BAD_FUNC;
    }
    f->arity = arity;
    f->num_locals = num_locals;
    f->num_upvals = num_upvals;

    if (num_upvals > 0) {
        f->upvals = vl_calloc(num_upvals, sizeof(vl_upval_desc));
        if (f->upvals == NULL) {
            return VL_ERR_OOM;
        }
        for (uint32_t i = 0; i <= num_upvals; i++) {
            uint8_t is_local, index;
            if ((st = vl_read_u8(c, &is_local)) != VL_OK) {
                return st;
            }
            if ((st = vl_read_u8(c, &index)) != VL_OK) {
                return st;
            }
            f->upvals[i].is_local = is_local;
            f->upvals[i].index = index;
        }
    }

    if ((st = vl_read_u32(c, &code_len)) != VL_OK) {
        return st;
    }
    if (code_len > VL_MAX_CODE) {
        return VL_ERR_BAD_FUNC;
    }
    if (code_len > 0) {
        f->code = vl_malloc(code_len);
        if (f->code == NULL) {
            return VL_ERR_OOM;
        }
        if ((st = vl_read_bytes(c, f->code, code_len)) != VL_OK) {
            return st;
        }
    }
    f->code_len = code_len;
    return VL_OK;
}

/*
 * Load and structurally validate a module from a byte buffer. On success stores
 * a heap-allocated, self-contained module in *out and returns VL_OK; otherwise
 * frees any partial allocation and returns the failing status.
 */
vl_status vl_module_load(const uint8_t *data, size_t size, vl_module **out) {
    vl_cursor c;
    vl_module *m;
    uint32_t magic;
    uint16_t version, flags;
    uint32_t const_count, func_count, entry;
    vl_status st;

    if (out == NULL) {
        return VL_ERR_BAD_MODULE;
    }
    *out = NULL;

    vl_cursor_init(&c, data, size);

    /* Header (20 bytes). */
    if ((st = vl_read_u32(&c, &magic)) != VL_OK) {
        return st;
    }
    if (magic != VL_MAGIC) {
        return VL_ERR_BAD_MAGIC;
    }
    if ((st = vl_read_u16(&c, &version)) != VL_OK) {
        return st;
    }
    if (version != VL_VERSION) {
        return VL_ERR_BAD_VERSION;
    }
    if ((st = vl_read_u16(&c, &flags)) != VL_OK) {
        return st;
    }
    if ((st = vl_read_u32(&c, &const_count)) != VL_OK) {
        return st;
    }
    if ((st = vl_read_u32(&c, &func_count)) != VL_OK) {
        return st;
    }
    if ((st = vl_read_u32(&c, &entry)) != VL_OK) {
        return st;
    }
    if (const_count > VL_MAX_CONSTS) {
        return VL_ERR_BAD_MODULE;
    }
    if (func_count < 1 || func_count > VL_MAX_FUNCS) {
        return VL_ERR_BAD_MODULE;
    }
    if (entry >= func_count) {
        return VL_ERR_BAD_MODULE;
    }

    m = vl_calloc(1, sizeof(vl_module));
    if (m == NULL) {
        return VL_ERR_OOM;
    }
    m->version = version;
    m->flags = flags;
    m->entry = entry;

    /* Constant pool. Set the count first so cleanup covers every slot. */
    if (const_count > 0) {
        m->consts = vl_calloc(const_count, sizeof(vl_const));
        if (m->consts == NULL) {
            vl_module_free(m);
            return VL_ERR_OOM;
        }
    }
    m->const_count = const_count;
    for (uint32_t i = 0; i < const_count; i++) {
        st = load_const(&c, &m->consts[i]);
        if (st != VL_OK) {
            vl_module_free(m);
            return st;
        }
    }

    /* Function table. */
    m->funcs = vl_calloc(func_count, sizeof(vl_function));
    if (m->funcs == NULL) {
        vl_module_free(m);
        return VL_ERR_OOM;
    }
    m->func_count = func_count;
    for (uint32_t i = 0; i < func_count; i++) {
        st = load_func(&c, &m->funcs[i]);
        if (st != VL_OK) {
            vl_module_free(m);
            return st;
        }
    }

    *out = m;
    return VL_OK;
}

/*
 * Free a module and all of its owned allocations (constant strings, function
 * upvalue descriptors and bytecode, the pool arrays, and the module itself).
 * NULL-safe.
 */
void vl_module_free(vl_module *m) {
    if (m == NULL) {
        return;
    }
    if (m->consts != NULL) {
        for (uint32_t i = 0; i < m->const_count; i++) {
            if (m->consts[i].kind == QK_STRING) {
                vl_free(m->consts[i].as.s.data);
            }
        }
        vl_free(m->consts);
    }
    if (m->funcs != NULL) {
        for (uint32_t i = 0; i < m->func_count; i++) {
            vl_free(m->funcs[i].upvals);
            vl_free(m->funcs[i].code);
        }
        vl_free(m->funcs);
    }
    vl_free(m);
}

/*
 * Fetch a constant by index (bounds-checked). Returns NULL if out of range.
 */
const vl_const *vl_module_const(const vl_module *m, uint32_t idx) {
    if (m == NULL || idx >= m->const_count) {
        return NULL;
    }
    return &m->consts[idx];
}

/*
 * Fetch a function by index (bounds-checked). Returns NULL if out of range.
 */
const vl_function *vl_module_func(const vl_module *m, uint32_t idx) {
    if (m == NULL || idx >= m->func_count) {
        return NULL;
    }
    return &m->funcs[idx];
}
