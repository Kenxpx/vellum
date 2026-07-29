/*
 * obj.c - reference-counted heap objects and their operations.
 *
 * Strings, arrays, maps, closures, and upvalues. vl_retain/vl_release manage
 * lifetimes; releasing the final reference frees the object after releasing the
 * values it owns. Arrays and maps own (retain) the values stored in them.
 */
#include "obj.h"

uint32_t vl_string_hash(const char *data, uint32_t len) {
    uint32_t h = 2166136261u;
    uint32_t i;
    for (i = 0; i < len; i++) {
        h ^= (uint8_t)data[i];
        h *= 16777619u;
    }
    return h;
}

static vl_obj *obj_alloc(size_t size, uint8_t otype) {
    vl_obj *o = (vl_obj *)vl_malloc(size);
    if (!o) {
        return NULL;
    }
    o->otype = otype;
    o->rc = 1;
    return o;
}

vl_string *vl_string_new(const char *data, uint32_t len) {
    vl_string *s = (vl_string *)obj_alloc(sizeof(vl_string), VL_OBJ_STRING);
    if (!s) {
        return NULL;
    }
    s->len = len;
    s->data = (char *)vl_malloc((size_t)len + 1);
    if (!s->data) {
        vl_free(s);
        return NULL;
    }
    if (len) {
        memcpy(s->data, data, len);
    }
    s->data[len] = '\0';
    s->hash = vl_string_hash(data, len);
    s->interned = 0;
    s->gc_mark = 0;
    return s;
}

vl_array *vl_array_new(void) {
    vl_array *a = (vl_array *)obj_alloc(sizeof(vl_array), VL_OBJ_ARRAY);
    if (!a) {
        return NULL;
    }
    a->items = NULL;
    a->len = 0;
    a->cap = 0;
    return a;
}

vl_map *vl_map_new(void) {
    vl_map *m = (vl_map *)obj_alloc(sizeof(vl_map), VL_OBJ_MAP);
    if (!m) {
        return NULL;
    }
    m->entries = NULL;
    m->count = 0;
    m->cap = 0;
    m->value_kind = VL_NIL;
    m->has_hint = 0;
    return m;
}

vl_closure *vl_closure_new(uint16_t func, uint8_t num_upvals) {
    vl_closure *c = (vl_closure *)obj_alloc(sizeof(vl_closure), VL_OBJ_CLOSURE);
    if (!c) {
        return NULL;
    }
    c->func = func;
    c->num_upvals = num_upvals;
    c->upvals = NULL;
    if (num_upvals > 0) {
        c->upvals = (vl_upvalue **)vl_calloc(num_upvals, sizeof(vl_upvalue *));
        if (!c->upvals) {
            vl_free(c);
            return NULL;
        }
    }
    return c;
}

vl_upvalue *vl_upvalue_new(vl_value *slot) {
    vl_upvalue *u = (vl_upvalue *)obj_alloc(sizeof(vl_upvalue), VL_OBJ_UPVALUE);
    if (!u) {
        return NULL;
    }
    u->loc = slot;
    u->closed = vl_nil();
    u->next = NULL;
    return u;
}

static void obj_free(vl_obj *o) {
    switch (o->otype) {
    case VL_OBJ_STRING:
        vl_free(((vl_string *)o)->data);
        break;
    case VL_OBJ_ARRAY: {
        vl_array *a = (vl_array *)o;
        uint32_t i;
        for (i = 0; i < a->len; i++) {
            vl_val_release(a->items[i]);
        }
        vl_free(a->items);
        break;
    }
    case VL_OBJ_MAP: {
        vl_map *m = (vl_map *)o;
        uint32_t i;
        for (i = 0; i < m->cap; i++) {
            if (m->entries[i].used) {
                vl_val_release(m->entries[i].key);
                vl_val_release(m->entries[i].val);
            }
        }
        vl_free(m->entries);
        break;
    }
    case VL_OBJ_CLOSURE: {
        vl_closure *c = (vl_closure *)o;
        uint8_t i;
        for (i = 0; i < c->num_upvals; i++) {
            if (c->upvals[i]) {
                vl_release((vl_obj *)c->upvals[i]);
            }
        }
        vl_free(c->upvals);
        break;
    }
    case VL_OBJ_UPVALUE: {
        vl_upvalue *u = (vl_upvalue *)o;
        /* A closed upvalue owns the value it copied; an open one does not. */
        if (u->loc == &u->closed) {
            vl_val_release(u->closed);
        }
        break;
    }
    default:
        break;
    }
    vl_free(o);
}

/* Interned strings are owned by the intern-table collector, not by refcounts. */
static int obj_is_interned(const vl_obj *o) {
    return o->otype == VL_OBJ_STRING && ((const vl_string *)o)->interned;
}

void vl_retain(vl_obj *o) {
    if (!o || obj_is_interned(o)) {
        return;
    }
    o->rc++;
}

void vl_release(vl_obj *o) {
    if (!o || o->rc == 0 || obj_is_interned(o)) {
        return;
    }
    if (--o->rc == 0) {
        obj_free(o);
    }
}

/* ------------------------------------------------------------------ array - */

static vl_status array_reserve(vl_array *a, uint32_t need) {
    uint32_t nc;
    size_t bytes;
    vl_value *ni;
    if (need <= a->cap) {
        return VL_OK;
    }
    nc = a->cap ? a->cap * 2 : 8;
    while (nc < need) {
        nc *= 2;
    }
    if (!vl_size_mul(nc, sizeof(vl_value), &bytes)) {
        return VL_ERR_OVERFLOW;
    }
    ni = (vl_value *)vl_realloc(a->items, bytes);
    if (!ni) {
        return VL_ERR_OOM;
    }
    a->items = ni;
    a->cap = nc;
    return VL_OK;
}

vl_status vl_array_push(vl_array *a, vl_value v) {
    vl_status st = array_reserve(a, a->len + 1);
    if (st != VL_OK) {
        return st;
    }
    vl_val_retain(v);
    a->items[a->len++] = v;
    return VL_OK;
}

vl_status vl_array_get(const vl_array *a, int64_t idx, vl_value *out) {
    if (idx < 0 || (uint64_t)idx >= a->len) {
        return VL_ERR_TRAP;
    }
    *out = a->items[idx];
    return VL_OK;
}

vl_status vl_array_set(vl_array *a, int64_t idx, vl_value v) {
    if (idx < 0 || (uint64_t)idx >= a->len) {
        return VL_ERR_TRAP;
    }
    vl_val_retain(v);                 /* retain the new value first ... */
    vl_val_release(a->items[idx]);    /* ... then release the old one   */
    a->items[idx] = v;
    return VL_OK;
}

/* -------------------------------------------------------------------- map - */

static uint32_t key_hash(vl_value k) {
    if (k.tag == VL_INT) {
        uint64_t x = (uint64_t)k.as.i;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return (uint32_t)x;
    }
    if (k.tag == VL_OBJ && k.as.o && k.as.o->otype == VL_OBJ_STRING) {
        return ((vl_string *)k.as.o)->hash;
    }
    return 0;
}

static int key_eq(vl_value a, vl_value b) {
    if (a.tag != b.tag) {
        return 0;
    }
    if (a.tag == VL_INT) {
        return a.as.i == b.as.i;
    }
    if (a.tag == VL_OBJ) {
        vl_string *sa = (vl_string *)a.as.o;
        vl_string *sb = (vl_string *)b.as.o;
        if (a.as.o == b.as.o) {
            return 1;
        }
        if (!sa || !sb || sa->head.otype != VL_OBJ_STRING ||
            sb->head.otype != VL_OBJ_STRING) {
            return 0;
        }
        return sa->len == sb->len && memcmp(sa->data, sb->data, sa->len) == 0;
    }
    return 0;
}

static int key_valid(vl_value k) {
    return k.tag == VL_INT ||
           (k.tag == VL_OBJ && k.as.o && k.as.o->otype == VL_OBJ_STRING);
}

/* Find the slot for key: returns index of a matching or the first free slot. */
static uint32_t map_slot(const vl_map *m, vl_value key, int *found) {
    uint32_t mask = m->cap - 1;
    uint32_t i = key_hash(key) & mask;
    for (;;) {
        if (!m->entries[i].used) {
            *found = 0;
            return i;
        }
        if (key_eq(m->entries[i].key, key)) {
            *found = 1;
            return i;
        }
        i = (i + 1) & mask;
    }
}

static vl_status map_grow(vl_map *m) {
    uint32_t nc = m->cap ? m->cap * 2 : 8;
    size_t bytes;
    vl_map_entry *ne;
    uint32_t i;
    if (!vl_size_mul(nc, sizeof(vl_map_entry), &bytes)) {
        return VL_ERR_OVERFLOW;
    }
    ne = (vl_map_entry *)vl_calloc(nc, sizeof(vl_map_entry));
    if (!ne) {
        return VL_ERR_OOM;
    }
    /* rehash existing entries into the new table */
    {
        vl_map_entry *old = m->entries;
        uint32_t oldcap = m->cap;
        m->entries = ne;
        m->cap = nc;
        for (i = 0; i < oldcap; i++) {
            if (old[i].used) {
                int found;
                uint32_t s = map_slot(m, old[i].key, &found);
                m->entries[s] = old[i];
            }
        }
        vl_free(old);
    }
    return VL_OK;
}

vl_status vl_map_set(vl_map *m, vl_value key, vl_value v) {
    int found;
    uint32_t s;
    if (!key_valid(key)) {
        return VL_ERR_TYPE;
    }
    if (m->cap == 0 || m->count * 10 >= m->cap * 7) {
        vl_status st = map_grow(m);
        if (st != VL_OK) {
            return st;
        }
    }
    s = map_slot(m, key, &found);
    if (found) {
        /* overwrite an existing key: swap the value, keeping refcounts sound */
        vl_val_retain(v);
        vl_val_release(m->entries[s].val);
        m->entries[s].val = v;
    } else {
        vl_val_retain(key);
        vl_val_retain(v);
        m->entries[s].key = key;
        m->entries[s].val = v;
        m->entries[s].used = 1;
        m->count++;
        /*
         * Record the value kind on the first insertion so reads can take a
         * fast typed path for homogeneous maps.
         */
        if (!m->has_hint) {
            m->value_kind = v.tag;
            m->has_hint = 1;
        }
    }
    return VL_OK;
}

int vl_map_get(const vl_map *m, vl_value key, vl_value *out) {
    int found;
    uint32_t s;
    if (m->cap == 0 || !key_valid(key)) {
        return 0;
    }
    s = map_slot(m, key, &found);
    if (!found) {
        return 0;
    }
    *out = m->entries[s].val;
    return 1;
}
