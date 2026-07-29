/*
 * value.h - the Vellum runtime value: a small tagged union of an immediate
 * (nil, bool, int, real) or a reference to a heap object (obj.h). Values are
 * copied freely on the stack; object references are reference-counted, so code
 * that stores a value into a container or local must retain/release around it
 * (see vl_val_retain / vl_val_release in obj.h).
 */
#ifndef VELLUM_VALUE_H
#define VELLUM_VALUE_H

#include "internal.h"

typedef struct vl_obj vl_obj;

enum vl_tag {
    VL_NIL = 0,
    VL_BOOL,
    VL_INT,
    VL_REAL,
    VL_OBJ
};

typedef struct vl_value {
    uint8_t tag;
    union {
        int64_t i; /* INT, and BOOL (0/1) */
        double d;  /* REAL                */
        vl_obj *o; /* OBJ                 */
    } as;
} vl_value;

static inline vl_value vl_nil(void) {
    vl_value v;
    v.tag = VL_NIL;
    v.as.i = 0;
    return v;
}
static inline vl_value vl_bool(int b) {
    vl_value v;
    v.tag = VL_BOOL;
    v.as.i = b ? 1 : 0;
    return v;
}
static inline vl_value vl_int(int64_t i) {
    vl_value v;
    v.tag = VL_INT;
    v.as.i = i;
    return v;
}
static inline vl_value vl_real(double d) {
    vl_value v;
    v.tag = VL_REAL;
    v.as.d = d;
    return v;
}
static inline vl_value vl_obj_val(vl_obj *o) {
    vl_value v;
    v.tag = VL_OBJ;
    v.as.o = o;
    return v;
}

static inline int vl_is_obj(vl_value v) { return v.tag == VL_OBJ; }

/* Truthiness: nil and false are falsey; everything else (incl. 0/empty) truthy. */
int vl_value_truthy(vl_value v);

/* Structural equality for immediates; identity for object references. */
int vl_value_equal(vl_value a, vl_value b);

#endif /* VELLUM_VALUE_H */
