/*
 * intern.c - the string intern table and its mark-and-sweep collector.
 *
 * Open-addressing table keyed by string content. Interned strings are owned
 * here rather than by reference counting; vl_intern_sweep frees the ones the
 * VM did not mark as reachable, and vl_intern_dispose frees the rest at
 * shutdown.
 */
#include "intern.h"

void vl_intern_init(vl_intern *t) {
    t->slots = NULL;
    t->cap = 0;
    t->count = 0;
}

/* Free the heap string object itself (data + struct). */
static void free_string(vl_string *s) {
    if (s) {
        vl_free(s->data);
        vl_free(s);
    }
}

void vl_intern_dispose(vl_intern *t) {
    size_t i;
    if (!t) {
        return;
    }
    for (i = 0; i < t->cap; i++) {
        if (t->slots[i]) {
            free_string(t->slots[i]);
        }
    }
    vl_free(t->slots);
    vl_intern_init(t);
}

static int str_matches(const vl_string *s, uint32_t hash, const char *text,
                       uint32_t len) {
    return s->hash == hash && s->len == len &&
           memcmp(s->data, text, len) == 0;
}

/* Insert an existing string into a table with a free slot (no growth). */
static void raw_insert(vl_string **slots, size_t cap, vl_string *s) {
    size_t i = (size_t)s->hash & (cap - 1);
    while (slots[i] != NULL) {
        i = (i + 1) & (cap - 1);
    }
    slots[i] = s;
}

static vl_status intern_grow(vl_intern *t) {
    size_t nc = t->cap ? t->cap * 2 : 16;
    vl_string **ns = (vl_string **)vl_calloc(nc, sizeof(vl_string *));
    size_t i;
    if (!ns) {
        return VL_ERR_OOM;
    }
    for (i = 0; i < t->cap; i++) {
        if (t->slots[i]) {
            raw_insert(ns, nc, t->slots[i]);
        }
    }
    vl_free(t->slots);
    t->slots = ns;
    t->cap = nc;
    return VL_OK;
}

static vl_string *make_interned(const char *text, uint32_t len) {
    vl_string *s = vl_string_new(text, len);
    if (s) {
        s->interned = 1;
        s->gc_mark = 0;
    }
    return s;
}

vl_string *vl_intern_get(vl_intern *t, const char *text, uint32_t len,
                         int *is_new) {
    uint32_t hash = vl_string_hash(text, len);
    size_t i;
    vl_string *s;

    if (is_new) {
        *is_new = 0;
    }
    if (t->cap == 0 || (t->count + 1) * 10 >= t->cap * 7) {
        if (intern_grow(t) != VL_OK) {
            return NULL;
        }
    }
    i = (size_t)hash & (t->cap - 1);
    while (t->slots[i] != NULL) {
        if (str_matches(t->slots[i], hash, text, len)) {
            return t->slots[i];
        }
        i = (i + 1) & (t->cap - 1);
    }
    s = make_interned(text, len);
    if (!s) {
        return NULL;
    }
    t->slots[i] = s;
    t->count++;
    if (is_new) {
        *is_new = 1;
    }
    return s;
}

size_t vl_intern_count(const vl_intern *t) { return t->count; }

void vl_intern_clear_marks(vl_intern *t) {
    size_t i;
    for (i = 0; i < t->cap; i++) {
        if (t->slots[i]) {
            t->slots[i]->gc_mark = 0;
        }
    }
}

void vl_intern_mark(vl_string *s) {
    if (s && s->interned) {
        s->gc_mark = 1;
    }
}

size_t vl_intern_sweep(vl_intern *t) {
    vl_string **ns;
    size_t newcap, i, freed = 0, kept = 0;
    if (t->cap == 0) {
        return 0;
    }
    /* Large tables that have gone sparse are compacted into a smaller one. */
    newcap = t->cap;
    if (t->cap >= 1024) {
        newcap = t->cap / 2;
    }
    ns = (vl_string **)vl_calloc(newcap, sizeof(vl_string *));
    if (!ns) {
        return 0; /* keep everything if we cannot rebuild */
    }
    for (i = 0; i < t->cap; i++) {
        vl_string *s = t->slots[i];
        if (!s) {
            continue;
        }
        if (s->gc_mark) {
            raw_insert(ns, t->cap, s);
            kept++;
        } else {
            free_string(s);
            freed++;
        }
    }
    vl_free(t->slots);
    t->slots = ns;
    t->cap = newcap;
    t->count = kept;
    return freed;
}
