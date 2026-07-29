/*
 * value.c - implementation of the Vellum runtime value predicates declared in
 * value.h: truthiness and equality over the tagged union.
 */
#include "value.h"
#include "internal.h"

/* Truthiness: nil is falsey, false is falsey; everything else is truthy. */
int vl_value_truthy(vl_value v) {
    switch (v.tag) {
    case VL_NIL:
        return 0;
    case VL_BOOL:
        return v.as.i != 0;
    default:
        return 1;
    }
}

/* Equality: structural for immediates, identity for object references. */
int vl_value_equal(vl_value a, vl_value b) {
    if (a.tag != b.tag) {
        return 0;
    }
    switch (a.tag) {
    case VL_NIL:
        return 1;
    case VL_BOOL:
    case VL_INT:
        return a.as.i == b.as.i;
    case VL_REAL:
        return a.as.d == b.as.d;
    case VL_OBJ:
        return a.as.o == b.as.o;
    default:
        return 0;
    }
}
