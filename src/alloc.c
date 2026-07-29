/*
 * alloc.c - allocation wrappers and overflow-checked size arithmetic.
 *
 * The default configuration forwards to the C standard library. When built with
 * -DVL_GUARD_ALLOC the four wrappers forward to the page-guarded debug allocator
 * in dbgalloc.c, which makes out-of-bounds and use-after-free accesses fault.
 */
#include "vellum/alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int vl_size_mul(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        return 0;
    }
    *out = a * b;
    return 1;
}

int vl_size_add(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a) {
        return 0;
    }
    *out = a + b;
    return 1;
}

#ifdef VL_GUARD_ALLOC

/* Provided by dbgalloc.c. */
void *vl_dbg_malloc(size_t size);
void *vl_dbg_realloc(void *ptr, size_t size);
void vl_dbg_free(void *ptr);

void *vl_malloc(size_t size) { return vl_dbg_malloc(size); }
void *vl_realloc(void *ptr, size_t size) { return vl_dbg_realloc(ptr, size); }
void vl_free(void *ptr) { vl_dbg_free(ptr); }

void *vl_calloc(size_t count, size_t size) {
    size_t total;
    void *p;
    if (!vl_size_mul(count, size, &total)) {
        return NULL;
    }
    p = vl_dbg_malloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

#else /* !VL_GUARD_ALLOC */

void *vl_malloc(size_t size) { return malloc(size); }
void *vl_calloc(size_t count, size_t size) { return calloc(count, size); }
void *vl_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void vl_free(void *ptr) { free(ptr); }

#endif /* VL_GUARD_ALLOC */
