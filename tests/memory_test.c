#include "core/memory.c"
#include "core/random.h"
#include "core/hash.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "memory test: %s at %d\n", #x, __LINE__); return 1; } } while (0)
int main(void) {
    Core_MemStats before = core_mem_stats();
    Core_Arena arena;
    CHECK(!core_arena_init(&arena, 0) && !arena.base);
    CHECK(!core_arena_init(&arena, UINT64_MAX) && !arena.base);
    CHECK(core_arena_init(&arena, CORE_MB(2)));
    CHECK(arena.committed == 0);
    u8 *p = core_arena_push(&arena, 19, 16);
    CHECK(p && (usize)p % 16 == 0);
    for (u32 i = 0; i < 19; i++) CHECK(p[i] == 0);
    memset(p, 7, 19);
    u64 used = arena.used;
    CHECK(!core_arena_push(&arena, UINT64_MAX, 1) && arena.used == used);
    CHECK(!core_arena_push(&arena, 1, 3) && arena.used == used);
    CHECK(!core_push_array(&arena, u64, UINT64_MAX) && arena.used == used);
    /* Alignment larger than an OS page must align the absolute address. */
    p = core_arena_push(&arena, 1, 65536);
    CHECK(p && (usize)p % 65536 == 0);
    Core_Temp tmp = core_temp_begin(&arena);
    CHECK(core_arena_push(&arena, CORE_MB(1), 1));
    core_temp_end(tmp);
    CHECK(arena.used == tmp.used);
    core_arena_reset(&arena);
    p = core_arena_push(&arena, 19, 16);
    for (u32 i = 0; i < 19; i++) CHECK(p[i] == 0);
    core_arena_release(&arena);
    CHECK(core_mem_stats().arena_committed == before.arena_committed);
    CHECK(!core_arena_push(&arena, 1, 1));

    CHECK(!core_heap_alloc(UINT64_MAX));
    CHECK(!core_heap_calloc(UINT64_MAX));
    p = core_heap_calloc(32);
    CHECK(p && (usize)p % _Alignof(long double) == 0);
    for (u32 i = 0; i < 32; i++) CHECK(p[i] == 0);
    memset(p, 7, 32);
    CHECK(!core_heap_realloc(p, UINT64_MAX) && p[31] == 7);
    u8 *grown = core_heap_realloc(p, 128);
    CHECK(grown && grown[31] == 7);
    CHECK(core_mem_stats().heap_live == before.heap_live + 128);
    core_heap_free(grown);
    CHECK(core_mem_stats().heap_live == before.heap_live);
    u32 rng = 1;
    CHECK(core_rng_next(&rng) == 270369);
    CHECK(core_rng_next(&rng) == 67634689);
    CHECK(core_rng_next(&rng) == 2647435461u);
    rng = 0;
    CHECK(core_rng_next(&rng) != 0);
    for (u32 i = 0; i < 10000; i++) { f32 x = core_rng_f32(&rng); CHECK(x >= 0 && x < 1); }
    CHECK(core_hash_bytes("hello", 5) == 0xa430d84680aabd0bULL);
    CHECK(core_hash_cstr("hello") == core_hash_bytes("hello", 5));
    CHECK(core_hash_bytes(0, 0) == 0xcbf29ce484222325ULL);
    puts("memory_test: ok");
    return 0;
}
