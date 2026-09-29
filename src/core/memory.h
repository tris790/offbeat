#ifndef CORE_MEMORY_H
#define CORE_MEMORY_H

#include "types.h"

/*
 * Arena allocator backed by virtual memory.
 *
 * On creation we reserve a large contiguous virtual address range but commit
 * physical pages lazily as the arena grows. This gives us stable pointers, no
 * reallocation, and effectively free "allocation" (a pointer bump) while only
 * paying for memory we actually touch.
 */

typedef struct Core_Arena {
    u8 *base;       /* start of the reserved range                 */
    u64 reserved;   /* total reserved bytes                        */
    u64 committed;  /* bytes currently committed (physical-backed) */
    u64 used;       /* bytes handed out                            */
} Core_Arena;

/* Reserve `reserve_size` bytes of address space. Returns false on failure. */
b32  core_arena_init(Core_Arena *arena, u64 reserve_size);
void core_arena_release(Core_Arena *arena);

/* Bump-allocate `size` bytes aligned to `align`. Zeroed. Returns 0 if OOM. */
void *core_arena_push(Core_Arena *arena, u64 size, u64 align);

/* Convenience typed helpers. */
#define core_push_struct(arena, T)      ((T *)core_arena_push((arena), sizeof(T), _Alignof(T)))
#define core_push_array(arena, T, n)    ((T *)core_arena_push((arena), sizeof(T) * (u64)(n), _Alignof(T)))

/* Save / restore the arena position to free everything pushed after the mark. */
typedef struct { Core_Arena *arena; u64 used; } Core_Temp;
Core_Temp core_temp_begin(Core_Arena *arena);
void      core_temp_end(Core_Temp temp);

/* Reset the arena to empty (keeps committed pages for reuse). */
void core_arena_reset(Core_Arena *arena);

/*
 * Tracked heap for short-lived or variable-size buffers (decoders, image
 * scratch). Thin wrapper over malloc that keeps a global byte count so the app
 * can report exactly how much memory it owns. Thread-safe.
 */
void *core_heap_alloc(u64 size);            /* not zeroed */
void *core_heap_calloc(u64 size);           /* zeroed     */
void *core_heap_realloc(void *ptr, u64 size);
void  core_heap_free(void *ptr);

/* Memory owned by the app: committed arena pages + live tracked heap bytes. */
typedef struct {
    u64 arena_committed;
    u64 heap_live;
    u64 heap_peak;
} Core_MemStats;
Core_MemStats core_mem_stats(void);

#endif /* CORE_MEMORY_H */
