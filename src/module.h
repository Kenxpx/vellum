/*
 * module.h - the on-disk Vellum module (.qbc) and its loader.
 *
 * A module is a header, a constant pool, and a function table. Each function
 * carries its arity, local-slot count, upvalue capture descriptors, and
 * bytecode. The loader validates structure and bounds against the VL_MAX_*
 * limits; the verifier (verify.h) then checks the bytecode itself. All
 * multi-byte integers are little-endian.
 */
#ifndef VELLUM_MODULE_H
#define VELLUM_MODULE_H

#include "cursor.h"
#include "internal.h"
#include "value.h"

#define VL_FOURCC(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))
#define VL_MAGIC VL_FOURCC('V', 'L', 'M', '1')
#define VL_VERSION 1

/* Structural limits bounding what a (possibly hostile) module may request. */
enum {
    VL_MAX_CONSTS = 1u << 16,
    VL_MAX_FUNCS = 1u << 16,
    VL_MAX_CODE = 1u << 20,   /* bytes of bytecode per function */
    VL_MAX_LOCALS = 0xFFFF,   /* num_locals is u16 on disk      */
    VL_MAX_UPVALS = 0xFF,     /* num_upvals is u8 on disk       */
    VL_MAX_STRING = 1u << 20
};

/* Runtime limits (see vm.h). */
enum {
    VL_MAX_STACK = 1u << 16,
    VL_MAX_FRAMES = 1024
};

enum vl_kconst {
    QK_INT = 0,
    QK_REAL = 1,
    QK_STRING = 2
};

typedef struct vl_const {
    uint8_t kind;
    union {
        int64_t i;
        double d;
        struct {
            char *data; /* owned; NUL-terminated */
            uint32_t len;
        } s;
    } as;
} vl_const;

typedef struct vl_upval_desc {
    uint8_t is_local; /* capture parent's local (1) or parent's upvalue (0) */
    uint8_t index;    /* which local slot or parent upvalue                 */
} vl_upval_desc;

typedef struct vl_function {
    uint8_t arity;
    uint16_t num_locals; /* includes arguments */
    uint8_t num_upvals;
    vl_upval_desc *upvals; /* num_upvals entries */
    uint8_t *code;         /* code_len bytes     */
    uint32_t code_len;
} vl_function;

typedef struct vl_module {
    uint16_t version;
    uint16_t flags;
    vl_const *consts;
    uint32_t const_count;
    vl_function *funcs;
    uint32_t func_count;
    uint32_t entry; /* index of the entry function */
} vl_module;

/* Load and structurally validate a module from a byte buffer. */
vl_status vl_module_load(const uint8_t *data, size_t size, vl_module **out);
void vl_module_free(vl_module *m);

/* Fetch a constant by index (bounds-checked). Returns NULL if out of range. */
const vl_const *vl_module_const(const vl_module *m, uint32_t idx);
/* Fetch a function by index (bounds-checked). Returns NULL if out of range. */
const vl_function *vl_module_func(const vl_module *m, uint32_t idx);

#endif /* VELLUM_MODULE_H */
