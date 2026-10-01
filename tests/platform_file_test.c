/* Concurrent atomic replacement must never truncate another writer's file. */
#include "core/memory.c"
#include "core/string.c"
#include "platform/platform_posix.c"

static _Atomic u32 finished;
typedef struct { const char *path; u8 value; b32 failed; } Writer;

static void write_many(void *user) {
    Writer *w = user;
    u8 payload[32768];
    memset(payload, w->value, sizeof(payload));
    for (u32 i = 0; i < 40; i++)
        if (!platform_file_write_all(w->path, payload, sizeof(payload))) w->failed = true;
    atomic_fetch_add(&finished, 1);
}

int main(void) {
    char dir[] = "/tmp/offbeat-file-test-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    char path[256];
    snprintf(path, sizeof(path), "%s/state", dir);
    Writer writers[4];
    Platform_Thread *threads[4] = {0};
    b32 failed = false;
    u8 initial[32768];
    memset(initial, 'A', sizeof(initial));
    if (!platform_file_write_all(path, initial, sizeof(initial))) return 1;
    for (u32 i = 0; i < 4; i++) {
        writers[i] = (Writer){path, (u8)('A' + i), false};
        threads[i] = platform_thread_start(write_many, &writers[i], "file-test");
        if (!threads[i]) { failed = true; atomic_fetch_add(&finished, 1); }
    }
    while (atomic_load(&finished) < 4) {
        Platform_File file = platform_file_open_read(path);
        u8 bytes[32768];
        if (platform_file_size(file) != sizeof(bytes) ||
            platform_file_read_at(file, 0, bytes, sizeof(bytes)) != sizeof(bytes)) failed = true;
        else {
            if (bytes[0] < 'A' || bytes[0] > 'D') failed = true;
            for (u64 i = 1; i < sizeof(bytes); i++)
                if (bytes[i] != bytes[0]) { failed = true; break; }
        }
        platform_file_close(file);
    }
    for (u32 i = 0; i < 4; i++) {
        platform_thread_join(threads[i]);
        failed |= writers[i].failed;
    }
    platform_remove_tree(dir);
    failed |= core_mem_stats().heap_live != 0;
    printf("platform_file_test: %s\n", failed ? "FAILED" : "ok");
    return failed ? 1 : 0;
}
