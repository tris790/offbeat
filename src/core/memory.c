#include "memory.h"

#include <sys/mman.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Atomic u64 g_arena_committed;
static _Atomic u64 g_heap_live;
static _Atomic u64 g_heap_peak;

#define CORE_ARENA_COMMIT_CHUNK CORE_MB(1)

static u64 core_page_size(void) {
    static u64 cached = 0;
    if (!cached) {
        long ps = sysconf(_SC_PAGESIZE);
        cached = (ps > 0) ? (u64)ps : 4096;
    }
    return cached;
}

b32 core_arena_init(Core_Arena *arena, u64 reserve_size) {
    u64 page = core_page_size();
    reserve_size = CORE_ALIGN_UP(reserve_size, page);

    /* Reserve address space without committing physical memory. */
    void *base = mmap(0, reserve_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        return false;
    }

    arena->base      = (u8 *)base;
    arena->reserved  = reserve_size;
    arena->committed = 0;
    arena->used      = 0;
    return true;
}

void core_arena_release(Core_Arena *arena) {
    if (arena->base) {
        atomic_fetch_sub(&g_arena_committed, arena->committed);
        munmap(arena->base, arena->reserved);
    }
    arena->base = 0;
    arena->reserved = arena->committed = arena->used = 0;
}

void *core_arena_push(Core_Arena *arena, u64 size, u64 align) {
    if (align == 0) align = 1;
    u64 pos = CORE_ALIGN_UP(arena->used, align);
    u64 new_used = pos + size;

    if (new_used > arena->reserved) {
        return 0; /* out of reserved address space */
    }

    if (new_used > arena->committed) {
        u64 page = core_page_size();
        u64 commit_to = CORE_ALIGN_UP(new_used, CORE_ARENA_COMMIT_CHUNK);
        commit_to = CORE_ALIGN_UP(commit_to, page);
        if (commit_to > arena->reserved) commit_to = arena->reserved;

        u64 commit_size = commit_to - arena->committed;
        if (mprotect(arena->base + arena->committed, commit_size,
                     PROT_READ | PROT_WRITE) != 0) {
            return 0;
        }
        atomic_fetch_add(&g_arena_committed, commit_size);
        arena->committed = commit_to;
    }

    void *result = arena->base + pos;
    arena->used = new_used;
    memset(result, 0, size);
    return result;
}

Core_Temp core_temp_begin(Core_Arena *arena) {
    return (Core_Temp){ .arena = arena, .used = arena->used };
}

void core_temp_end(Core_Temp temp) {
    temp.arena->used = temp.used;
}

void core_arena_reset(Core_Arena *arena) {
    arena->used = 0;
}

/* ---- tracked heap ----
   Each block carries a 16-byte header holding its size so free/realloc can
   keep the live counter exact without asking the allocator. */

#define HEAP_HDR 16

static void heap_note_alloc(u64 size) {
    u64 live = atomic_fetch_add(&g_heap_live, size) + size;
    u64 peak = atomic_load(&g_heap_peak);
    while (live > peak && !atomic_compare_exchange_weak(&g_heap_peak, &peak, live)) {}
}

void *core_heap_alloc(u64 size) {
    u8 *p = malloc(size + HEAP_HDR);
    if (!p) return 0;
    *(u64 *)p = size;
    heap_note_alloc(size);
    return p + HEAP_HDR;
}

void *core_heap_calloc(u64 size) {
    u8 *p = calloc(1, size + HEAP_HDR);
    if (!p) return 0;
    *(u64 *)p = size;
    heap_note_alloc(size);
    return p + HEAP_HDR;
}

void *core_heap_realloc(void *ptr, u64 size) {
    if (!ptr) return core_heap_alloc(size);
    u8 *base = (u8 *)ptr - HEAP_HDR;
    u64 old = *(u64 *)base;
    u8 *p = realloc(base, size + HEAP_HDR);
    if (!p) return 0;
    *(u64 *)p = size;
    atomic_fetch_sub(&g_heap_live, old);
    heap_note_alloc(size);
    return p + HEAP_HDR;
}

void core_heap_free(void *ptr) {
    if (!ptr) return;
    u8 *base = (u8 *)ptr - HEAP_HDR;
    atomic_fetch_sub(&g_heap_live, *(u64 *)base);
    free(base);
}

Core_MemStats core_mem_stats(void) {
    return (Core_MemStats){
        .arena_committed = atomic_load(&g_arena_committed),
        .heap_live       = atomic_load(&g_heap_live),
        .heap_peak       = atomic_load(&g_heap_peak),
    };
}
