/*
 * intern.h - the string intern table and its collector.
 *
 * String constants are interned so identical text shares one heap object. The
 * table owns its strings (they are excluded from reference counting) and, once
 * the number of distinct interned strings crosses a threshold, the VM runs a
 * mark-and-sweep over the table: strings still reachable from the live roots are
 * kept, the rest are freed. This keeps the table bounded during long-running
 * programs that mint many distinct strings.
 */
#ifndef VELLUM_INTERN_H
#define VELLUM_INTERN_H

#include "internal.h"
#include "obj.h"

/* Run the collector once the table holds this many distinct strings. */
#define VL_INTERN_GC_THRESHOLD 256

typedef struct vl_intern {
    vl_string **slots; /* open-addressing table; NULL = empty */
    size_t cap;
    size_t count; /* distinct interned strings */
} vl_intern;

void vl_intern_init(vl_intern *t);
void vl_intern_dispose(vl_intern *t); /* frees every interned string + table */

/*
 * Return the interned string for text[0,len), creating it (interned, unmarked)
 * if absent. Sets *is_new to 1 when a new string was created. Returns NULL only
 * on allocation failure.
 */
vl_string *vl_intern_get(vl_intern *t, const char *text, uint32_t len,
                         int *is_new);

size_t vl_intern_count(const vl_intern *t);

/* Collector primitives (driven by the VM, which supplies the roots). */
void vl_intern_clear_marks(vl_intern *t);
void vl_intern_mark(vl_string *s);   /* mark one string if it is interned */
size_t vl_intern_sweep(vl_intern *t); /* free unmarked strings; return freed */

#endif /* VELLUM_INTERN_H */
