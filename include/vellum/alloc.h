/*
 * vellum/alloc.h - allocation wrappers.
 *
 * Every heap allocation in the library goes through these four functions so
 * that an alternate allocator can be swapped in without touching call sites.
 * By default they forward to the C standard library. When the library is built
 * with -DVL_GUARD_ALLOC they forward instead to a page-guarded debug allocator
 * (src/dbgalloc.c) that places an inaccessible guard page immediately after
 * every allocation and releases pages on free, so out-of-bounds accesses and
 * use-after-free faults become deterministic crashes. That mode is used by the
 * local test and verification builds; production builds use the default.
 */
#ifndef VELLUM_ALLOC_H
#define VELLUM_ALLOC_H

#include <stddef.h>

void *vl_malloc(size_t size);
void *vl_calloc(size_t count, size_t size);
void *vl_realloc(void *ptr, size_t size);
void vl_free(void *ptr);

/*
 * vl_size_mul / vl_size_add - overflow-checked size arithmetic helpers.
 * They return 1 on success and store the result in *out, or 0 if the
 * computation would overflow size_t (in which case *out is left unspecified).
 * Call sites that size an allocation from untrusted counts should use these.
 */
int vl_size_mul(size_t a, size_t b, size_t *out);
int vl_size_add(size_t a, size_t b, size_t *out);

#endif /* VELLUM_ALLOC_H */
