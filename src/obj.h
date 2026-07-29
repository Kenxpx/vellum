/*
 * obj.h - the reference-counted heap objects of the Vellum VM.
 *
 * Every heap object begins with a vl_obj header carrying its type and reference
 * count. vl_retain/vl_release manage the count; releasing the last reference
 * frees the object after releasing the values it owns. Strings, arrays, maps,
 * closures, and upvalues are the object kinds.
 */
#ifndef VELLUM_OBJ_H
#define VELLUM_OBJ_H

#include "internal.h"
#include "value.h"

enum vl_otype {
    VL_OBJ_STRING = 1,
    VL_OBJ_ARRAY,
    VL_OBJ_MAP,
    VL_OBJ_CLOSURE,
    VL_OBJ_UPVALUE
};

struct vl_obj {
    uint8_t otype;
    uint32_t rc;
};

typedef struct vl_string {
    vl_obj head;
    uint32_t len;
    uint32_t hash;
    char *data;        /* len bytes, NUL-terminated for convenience         */
    uint8_t interned;  /* owned by the intern-table collector, not refcount */
    uint8_t gc_mark;   /* reachability mark, used by the intern collector    */
} vl_string;

typedef struct vl_array {
    vl_obj head;
    vl_value *items;
    uint32_t len;
    uint32_t cap;
} vl_array;

typedef struct vl_map_entry {
    vl_value key;
    vl_value val;
    uint8_t used;
} vl_map_entry;

typedef struct vl_map {
    vl_obj head;
    vl_map_entry *entries;
    uint32_t count;
    uint32_t cap;
    uint8_t value_kind; /* cached tag of stored values, for a fast typed read */
    uint8_t has_hint;   /* whether value_kind is populated                    */
} vl_map;

typedef struct vl_upvalue {
    vl_obj head;
    vl_value *loc;          /* points at a live stack slot while "open"       */
    vl_value closed;        /* holds the value once "closed"; loc -> &closed  */
    struct vl_upvalue *next;/* intrusive list of open upvalues (VM-managed)   */
} vl_upvalue;

typedef struct vl_closure {
    vl_obj head;
    uint16_t func;       /* index into the module function table */
    uint8_t num_upvals;
    vl_upvalue **upvals; /* num_upvals entries                   */
} vl_closure;

/* Reference counting. vl_release frees at rc==0, releasing owned values. */
void vl_retain(vl_obj *o);
void vl_release(vl_obj *o);

static inline void vl_val_retain(vl_value v) {
    if (v.tag == VL_OBJ && v.as.o) {
        vl_retain(v.as.o);
    }
}
static inline void vl_val_release(vl_value v) {
    if (v.tag == VL_OBJ && v.as.o) {
        vl_release(v.as.o);
    }
}

/* Constructors return an object with rc == 1, or NULL on OOM. */
vl_string *vl_string_new(const char *data, uint32_t len);
vl_array *vl_array_new(void);
vl_map *vl_map_new(void);
vl_closure *vl_closure_new(uint16_t func, uint8_t num_upvals);
vl_upvalue *vl_upvalue_new(vl_value *slot);

/* Array operations. Negative or out-of-range indices return VL_ERR_TRAP. */
vl_status vl_array_push(vl_array *a, vl_value v); /* retains v */
vl_status vl_array_get(const vl_array *a, int64_t idx, vl_value *out);
vl_status vl_array_set(vl_array *a, int64_t idx, vl_value v);

/* Map operations. Keys may be int or string. vl_map_get returns 1 on hit. */
vl_status vl_map_set(vl_map *m, vl_value key, vl_value v);
int vl_map_get(const vl_map *m, vl_value key, vl_value *out);

/* String hashing (FNV-1a), used for map keys and interning. */
uint32_t vl_string_hash(const char *data, uint32_t len);

#endif /* VELLUM_OBJ_H */
