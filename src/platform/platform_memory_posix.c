#include "memory.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <sys/mman.h>
#include <unistd.h>

u64 platform_memory_page_size(void) {
    long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? (u64)size : 4096;
}

void *platform_memory_reserve(u64 size) {
    void *base = mmap(0, size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return base == MAP_FAILED ? 0 : base;
}

b32 platform_memory_commit(void *base, u64 size) {
    return mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
}

void platform_memory_release(void *base, u64 size) {
    munmap(base, size);
}

/* glibc grows extra arenas when threads contend (cover workers, audio) and
   raises its mmap threshold after large frees, so freed multi-MB decode
   buffers stayed resident in each arena (~400 MB/day of free-but-retained
   memory while playing). One arena plus a fixed threshold returns large
   buffers to the OS on free. */
b32 platform_memory_tune_heap(void) {
#ifdef __GLIBC__
    b32 ok = mallopt(M_ARENA_MAX, 1) == 1;
    ok = (mallopt(M_MMAP_THRESHOLD, 256 * 1024) == 1) && ok;
    return ok;
#else
    return true;
#endif
}
