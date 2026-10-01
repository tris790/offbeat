#ifndef PLATFORM_MEMORY_H
#define PLATFORM_MEMORY_H

#include "../core/types.h"

/* Small OS boundary for the reusable arena allocator. Sizes are page-aligned.
   Reserve returns inaccessible address space; commit enables reads/writes.
   A platform port implements these without changing core/memory.c. */
u64   platform_memory_page_size(void);
void *platform_memory_reserve(u64 size);
b32   platform_memory_commit(void *base, u64 size);
void  platform_memory_release(void *base, u64 size);

#endif
