#include "memory.h"

#include "../platform/memory.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic u64 g_arena_committed;
static _Atomic u64 g_heap_live;
static _Atomic u64 g_heap_peak;

#define CORE_ARENA_COMMIT_CHUNK CORE_MB(1)

b32 core_arena_init(Core_Arena *arena, u64 reserve_size) {
    *arena = (Core_Arena){0};
    u64 page = platform_memory_page_size();
    if (!reserve_size || reserve_size > SIZE_MAX - (page - 1)) return false;
    reserve_size = CORE_ALIGN_UP(reserve_size, page);
    void *base = platform_memory_reserve(reserve_size);
    if (!base) return false;
    arena->base = base;
    arena->reserved = reserve_size;
    return true;
}

void core_arena_release(Core_Arena *arena) {
    if (arena->base) {
        atomic_fetch_sub(&g_arena_committed, arena->committed);
        platform_memory_release(arena->base, arena->reserved);
    }
    arena->base = 0;
    arena->reserved = arena->committed = arena->used = 0;
}

void *core_arena_push(Core_Arena *arena, u64 size, u64 align) {
    if (!arena->base) return 0;
    if (align == 0) align = 1;
    if ((align & (align - 1)) || align > SIZE_MAX || arena->used > arena->reserved) return 0;
    u64 misalignment = ((usize)arena->base + arena->used) & (align - 1);
    u64 padding = misalignment ? align - misalignment : 0;
    if (padding > arena->reserved - arena->used) return 0;
    u64 pos = arena->used + padding;
    if (size > arena->reserved - pos) return 0;
    u64 new_used = pos + size;

    if (new_used > arena->committed) {
        u64 page = platform_memory_page_size();
        u64 chunk = CORE_MAX(CORE_ARENA_COMMIT_CHUNK, page);
        u64 extra = (chunk - (new_used & (chunk - 1))) & (chunk - 1);
        u64 commit_to = extra > arena->reserved - new_used ? arena->reserved : new_used + extra;
        u64 commit_size = commit_to - arena->committed;
        if (!platform_memory_commit(arena->base + arena->committed, commit_size)) return 0;
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

typedef union { u64 size; long double align; } Core_HeapHeader;
#define HEAP_HDR sizeof(Core_HeapHeader)

static void core_heap_note_alloc(u64 size) {
    u64 live = atomic_fetch_add(&g_heap_live, size) + size;
    u64 peak = atomic_load(&g_heap_peak);
    while (live > peak && !atomic_compare_exchange_weak(&g_heap_peak, &peak, live)) {}
}

void *core_heap_alloc(u64 size) {
    if (size > SIZE_MAX - HEAP_HDR) return 0;
    u8 *p = malloc(size + HEAP_HDR);
    if (!p) return 0;
    *(u64 *)p = size;
    core_heap_note_alloc(size);
    return p + HEAP_HDR;
}

void *core_heap_calloc(u64 size) {
    if (size > SIZE_MAX - HEAP_HDR) return 0;
    u8 *p = calloc(1, size + HEAP_HDR);
    if (!p) return 0;
    *(u64 *)p = size;
    core_heap_note_alloc(size);
    return p + HEAP_HDR;
}

void *core_heap_realloc(void *ptr, u64 size) {
    if (!ptr) return core_heap_alloc(size);
    if (size > SIZE_MAX - HEAP_HDR) return 0;
    u8 *base = (u8 *)ptr - HEAP_HDR;
    u64 old = *(u64 *)base;
    u8 *p = realloc(base, size + HEAP_HDR);
    if (!p) return 0;
    *(u64 *)p = size;
    atomic_fetch_sub(&g_heap_live, old);
    core_heap_note_alloc(size);
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
