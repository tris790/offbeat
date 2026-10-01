#include "memory.h"

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
