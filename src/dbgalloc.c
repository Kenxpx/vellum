/*
 * dbgalloc.c - a page-guarded debug allocator.
 *
 * Enabled only with -DVL_GUARD_ALLOC. Every allocation is placed so that its
 * last byte abuts an inaccessible guard page, and freeing releases the pages
 * entirely (realloc frees the old block). Reading or writing past the end of an
 * allocation, or touching a freed/realloc'd block, therefore faults
 * immediately. It is a coarse local stand-in for AddressSanitizer used by the
 * test and verification builds; production builds use the standard allocator.
 *
 * A small side registry (backed by the system allocator, so it is not itself
 * guarded) maps each returned pointer to its underlying reservation.
 */
#ifdef VL_GUARD_ALLOC

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

typedef struct dbg_entry {
    uintptr_t user;   /* pointer handed to the caller (0 = empty slot) */
    uintptr_t region; /* base of the underlying reservation           */
    size_t reserve;   /* total reserved bytes                         */
    size_t size;      /* bytes requested by the caller                */
} dbg_entry;

static dbg_entry *g_reg;
static size_t g_reg_cap;
static size_t g_reg_count;

static size_t dbg_pagesize(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (size_t)si.dwPageSize;
#else
    long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t)ps : 4096u;
#endif
}

static size_t dbg_round_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

static void dbg_die(const char *msg, const void *p) {
    fprintf(stderr, "dbgalloc: %s (%p)\n", msg, p);
    fflush(stderr);
    abort();
}

static void dbg_reg_grow(void) {
    size_t new_cap = g_reg_cap ? g_reg_cap * 2 : 4096;
    dbg_entry *nb = (dbg_entry *)calloc(new_cap, sizeof(dbg_entry));
    size_t i;
    if (!nb) {
        dbg_die("registry out of memory", NULL);
    }
    for (i = 0; i < g_reg_cap; i++) {
        if (g_reg[i].user != 0) {
            size_t h = (size_t)((g_reg[i].user >> 4) * 11400714819323198485ull) &
                       (new_cap - 1);
            while (nb[h].user != 0) {
                h = (h + 1) & (new_cap - 1);
            }
            nb[h] = g_reg[i];
        }
    }
    free(g_reg);
    g_reg = nb;
    g_reg_cap = new_cap;
}

static void dbg_reg_put(uintptr_t user, uintptr_t region, size_t reserve,
                        size_t size) {
    size_t h;
    if (g_reg_count * 10 >= g_reg_cap * 7) {
        dbg_reg_grow();
    }
    h = (size_t)((user >> 4) * 11400714819323198485ull) & (g_reg_cap - 1);
    while (g_reg[h].user != 0) {
        h = (h + 1) & (g_reg_cap - 1);
    }
    g_reg[h].user = user;
    g_reg[h].region = region;
    g_reg[h].reserve = reserve;
    g_reg[h].size = size;
    g_reg_count++;
}

/* Find and remove the entry for `user`, copying it into *out. */
static int dbg_reg_take(uintptr_t user, dbg_entry *out) {
    size_t h, j, k;
    if (g_reg_cap == 0) {
        return 0;
    }
    h = (size_t)((user >> 4) * 11400714819323198485ull) & (g_reg_cap - 1);
    while (g_reg[h].user != 0) {
        if (g_reg[h].user == user) {
            *out = g_reg[h];
            /* Remove and re-cluster the following run (backward-shift delete). */
            g_reg[h].user = 0;
            g_reg_count--;
            j = h;
            for (k = (h + 1) & (g_reg_cap - 1); g_reg[k].user != 0;
                 k = (k + 1) & (g_reg_cap - 1)) {
                size_t home =
                    (size_t)((g_reg[k].user >> 4) * 11400714819323198485ull) &
                    (g_reg_cap - 1);
                int between = (j < k) ? (home > j && home <= k)
                                      : (home > j || home <= k);
                if (!between) {
                    g_reg[j] = g_reg[k];
                    g_reg[k].user = 0;
                    j = k;
                }
            }
            return 1;
        }
        h = (h + 1) & (g_reg_cap - 1);
    }
    return 0;
}

static void *dbg_map(size_t reserve) {
#if defined(_WIN32)
    return VirtualAlloc(NULL, reserve, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap(NULL, reserve, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void dbg_guard(void *guard_page, size_t pagesize) {
#if defined(_WIN32)
    DWORD old;
    VirtualProtect(guard_page, pagesize, PAGE_NOACCESS, &old);
#else
    mprotect(guard_page, pagesize, PROT_NONE);
#endif
}

/*
 * Quarantine a freed reservation: keep the address range reserved but deny all
 * access, so the memory is never handed back out and any later read or write
 * faults. This mirrors AddressSanitizer's quarantine and is what lets it catch
 * a use-after-free even when a subsequent allocation would otherwise reuse the
 * same address. Address space is not reclaimed, which is acceptable for the
 * short-lived verification runs this allocator is built for.
 */
static void dbg_unmap(void *region, size_t reserve) {
#if defined(_WIN32)
    DWORD old;
    VirtualProtect(region, reserve, PAGE_NOACCESS, &old);
#else
    mprotect(region, reserve, PROT_NONE);
#endif
}

void *vl_dbg_malloc(size_t size) {
    size_t pagesize = dbg_pagesize();
    size_t data_bytes, reserve;
    uint8_t *region, *user;
    if (size == 0) {
        size = 1;
    }
    data_bytes = dbg_round_up(size, pagesize);
    reserve = data_bytes + pagesize; /* trailing guard page */
    region = (uint8_t *)dbg_map(reserve);
    if (!region) {
        return NULL;
    }
    dbg_guard(region + data_bytes, pagesize);
    user = region + data_bytes - size; /* end-align to the guard page */
    dbg_reg_put((uintptr_t)user, (uintptr_t)region, reserve, size);
    return user;
}

void vl_dbg_free(void *ptr) {
    dbg_entry e;
    if (!ptr) {
        return;
    }
    if (!dbg_reg_take((uintptr_t)ptr, &e)) {
        dbg_die("invalid or double free", ptr);
    }
    dbg_unmap((void *)e.region, e.reserve);
}

void *vl_dbg_realloc(void *ptr, size_t size) {
    dbg_entry e;
    void *np;
    if (!ptr) {
        return vl_dbg_malloc(size);
    }
    if (size == 0) {
        vl_dbg_free(ptr);
        return NULL;
    }
    /* Peek the old size without removing (take then re-put would race a fault). */
    {
        size_t h;
        size_t old_size = 0;
        int found = 0;
        if (g_reg_cap) {
            h = (size_t)(((uintptr_t)ptr >> 4) * 11400714819323198485ull) &
                (g_reg_cap - 1);
            while (g_reg[h].user != 0) {
                if (g_reg[h].user == (uintptr_t)ptr) {
                    old_size = g_reg[h].size;
                    found = 1;
                    break;
                }
                h = (h + 1) & (g_reg_cap - 1);
            }
        }
        if (!found) {
            dbg_die("realloc of invalid pointer", ptr);
        }
        np = vl_dbg_malloc(size);
        if (!np) {
            return NULL;
        }
        memcpy(np, ptr, old_size < size ? old_size : size);
        /* Release the old reservation: any pointer still into it now faults. */
        vl_dbg_free(ptr);
        return np;
    }
}

#else
/* Non-guard builds: this translation unit is intentionally empty. */
typedef int vl_dbgalloc_translation_unit_not_empty;
#endif /* VL_GUARD_ALLOC */
