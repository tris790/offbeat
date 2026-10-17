/*
 * leaktrace: LD_PRELOAD malloc tracer for slow leaks outside the tracked core
 * heap (libc users, libwayland, EGL/GL drivers). Development tool only.
 *
 *   gcc -O2 -g -shared -fPIC tools/leaktrace.c -o build/leaktrace.so
 *   LEAKTRACE_OUT=/tmp/lt [LEAKTRACE_INTERVAL=60] \
 *       LD_PRELOAD=$PWD/build/leaktrace.so ./build/offbeat
 *
 * Every interval it writes LEAKTRACE_OUT.<seconds>: the allocation stacks with
 * the most live bytes (count, bytes, frames as module+offset). A leak is a
 * stack whose live bytes keep growing between dumps; tools/leaktrace.py diffs
 * two dumps and symbolizes them with addr2line.
 *
 * Bookkeeping lives in mmap'd tables, so the tracer never re-enters malloc.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);
extern void  __libc_free(void *);

#define LT_DEPTH  16
#define LT_SKIP   2                  /* tracer frames */
#define LT_LIVE   (1u << 23)         /* live allocation slots (power of two) */
#define LT_STACKS (1u << 16)         /* distinct stacks (power of two)       */
#define LT_TOP    40

typedef struct { uintptr_t ptr; uint64_t size; uint32_t stack; } Lt_Live;
typedef struct { uint64_t hash, count, bytes; uint32_t depth; void *frames[LT_DEPTH]; } Lt_Stack;

static Lt_Live  *g_live;
static Lt_Stack *g_stacks;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static __thread int g_inside;
static int g_ready, g_failed;
static double g_start, g_next, g_every = 60;
static const char *g_out;
static uint64_t g_calls, g_dropped;
static int g_dumping;

static double lt_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void lt_init(void) {
    g_live = mmap(0, sizeof(Lt_Live) * LT_LIVE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_stacks = mmap(0, sizeof(Lt_Stack) * LT_STACKS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_live == MAP_FAILED || g_stacks == MAP_FAILED) { g_failed = 1; return; }
    g_out = getenv("LEAKTRACE_OUT");
    if (!g_out) g_out = "/tmp/leaktrace";
    const char *every = getenv("LEAKTRACE_INTERVAL");
    if (every && atof(every) > 0) g_every = atof(every);
    g_start = lt_now();
    g_next = g_start + g_every;
    g_ready = 1;
}

static uint64_t lt_mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return x;
}

static uint32_t lt_stack_id(void **frames, int depth) {
    uint64_t h = (uint64_t)depth;
    for (int i = 0; i < depth; i++) h = lt_mix(h ^ (uint64_t)(uintptr_t)frames[i]);
    if (!h) h = 1;
    for (uint32_t i = (uint32_t)h & (LT_STACKS - 1), n = 0; n < LT_STACKS; i = (i + 1) & (LT_STACKS - 1), n++) {
        Lt_Stack *s = &g_stacks[i];
        if (s->hash == h) return i;
        if (!s->hash) {
            s->hash = h;
            s->depth = (uint32_t)depth;
            memcpy(s->frames, frames, sizeof(void *) * (size_t)depth);
            return i;
        }
    }
    return UINT32_MAX;
}

static Lt_Live *lt_slot(uintptr_t p, int insert) {
    for (uint32_t i = (uint32_t)lt_mix(p) & (LT_LIVE - 1), n = 0; n < LT_LIVE; i = (i + 1) & (LT_LIVE - 1), n++) {
        Lt_Live *l = &g_live[i];
        if (l->ptr == p) return l;
        if (!l->ptr) return insert ? l : 0;
    }
    return 0;
}

/* Backward-shift deletion keeps probe chains intact without tombstones. */
static void lt_remove(Lt_Live *l) {
    uint32_t i = (uint32_t)(l - g_live);
    for (;;) {
        g_live[i].ptr = 0;
        uint32_t j = i;
        for (;;) {
            j = (j + 1) & (LT_LIVE - 1);
            if (!g_live[j].ptr) return;
            uint32_t home = (uint32_t)lt_mix(g_live[j].ptr) & (LT_LIVE - 1);
            if (((j - home) & (LT_LIVE - 1)) >= ((j - i) & (LT_LIVE - 1))) break;
        }
        g_live[i] = g_live[j];
        i = j;
    }
}

static int lt_cmp(const void *a, const void *b) {
    uint64_t x = g_stacks[*(const uint32_t *)a].bytes, y = g_stacks[*(const uint32_t *)b].bytes;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Called with g_lock held: snapshot the top stacks so symbolization and file
   output (dladdr takes the loader lock) run after the tracer lock is released. */
static Lt_Stack g_snap[LT_TOP];
static uint32_t g_snap_n;
static uint64_t g_snap_bytes, g_snap_count, g_snap_stacks;
static double   g_snap_t;
/* A few live allocations of the top stacks, as raw words: pointers into
   library data (e.g. a wl_closure's wl_message) are symbolized when written. */
#define LT_SAMPLE_STACKS 4
#define LT_SAMPLES       3
#define LT_SAMPLE_WORDS  6
static uint64_t g_sample[LT_SAMPLE_STACKS][LT_SAMPLES][LT_SAMPLE_WORDS];
static uint32_t g_sample_n[LT_SAMPLE_STACKS];
static uint32_t g_sample_stack[LT_SAMPLE_STACKS]; /* most live allocations */

static void lt_snapshot(double now) {
    static uint32_t order[LT_STACKS];
    uint32_t n = 0;
    g_snap_bytes = g_snap_count = 0;
    for (uint32_t i = 0; i < LT_STACKS; i++) {
        if (!g_stacks[i].hash || !g_stacks[i].count) continue;
        order[n++] = i;
        g_snap_bytes += g_stacks[i].bytes;
        g_snap_count += g_stacks[i].count;
    }
    qsort(order, n, sizeof(order[0]), lt_cmp);
    g_snap_stacks = n;
    g_snap_n = n < LT_TOP ? n : LT_TOP;
    for (uint32_t k = 0; k < g_snap_n; k++) g_snap[k] = g_stacks[order[k]];
    g_snap_t = now - g_start;

    /* Entries are removed before free() releases them, so every pointer still
       in the table is readable while g_lock is held. */
    memset(g_sample_n, 0, sizeof(g_sample_n));
    uint32_t want = n < LT_SAMPLE_STACKS ? n : LT_SAMPLE_STACKS;
    for (uint32_t k = 0; k < want; k++) {
        uint32_t best = UINT32_MAX;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t id = order[i], taken = 0;
            for (uint32_t j = 0; j < k; j++) taken |= g_sample_stack[j] == id;
            if (!taken && (best == UINT32_MAX || g_stacks[id].count > g_stacks[best].count)) best = id;
        }
        g_sample_stack[k] = best;
    }
    for (uint32_t i = 0; i < LT_LIVE && want; i++) {
        Lt_Live *l = &g_live[i];
        if (!l->ptr || l->size < sizeof(uint64_t) * LT_SAMPLE_WORDS) continue;
        for (uint32_t k = 0; k < want; k++) {
            if (l->stack != g_sample_stack[k] || g_sample_n[k] == LT_SAMPLES) continue;
            memcpy(g_sample[k][g_sample_n[k]++], (void *)l->ptr, sizeof(g_sample[k][0]));
        }
    }
}

static void lt_write_snapshot(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s.%06.0f", g_out, g_snap_t);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char line[1024];
    int len = snprintf(line, sizeof(line), "# t=%.0f live_bytes=%llu live_allocs=%llu stacks=%llu calls=%llu dropped=%llu\n",
                       g_snap_t, (unsigned long long)g_snap_bytes, (unsigned long long)g_snap_count,
                       (unsigned long long)g_snap_stacks, (unsigned long long)g_calls, (unsigned long long)g_dropped);
    write(fd, line, (size_t)len);
    for (uint32_t k = 0; k < g_snap_n; k++) {
        Lt_Stack *s = &g_snap[k];
        len = snprintf(line, sizeof(line), "stack %016llx count=%llu bytes=%llu\n", (unsigned long long)s->hash,
                       (unsigned long long)s->count, (unsigned long long)s->bytes);
        write(fd, line, (size_t)len);
        for (uint32_t f = 0; f < s->depth; f++) {
            Dl_info info = {0};
            if (dladdr(s->frames[f], &info) && info.dli_fname) {
                len = snprintf(line, sizeof(line), "  %s+0x%llx %s\n", info.dli_fname,
                               (unsigned long long)((uintptr_t)s->frames[f] - (uintptr_t)info.dli_fbase),
                               info.dli_sname ? info.dli_sname : "?");
            } else {
                len = snprintf(line, sizeof(line), "  ?+%p\n", s->frames[f]);
            }
            write(fd, line, (size_t)len);
        }
    }
    for (uint32_t k = 0; k < LT_SAMPLE_STACKS && k < g_snap_stacks; k++) {
        len = snprintf(line, sizeof(line), "samples %016llx count=%llu\n",
                       (unsigned long long)g_stacks[g_sample_stack[k]].hash,
                       (unsigned long long)g_stacks[g_sample_stack[k]].count);
        write(fd, line, (size_t)len);
        for (uint32_t m = 0; m < g_sample_n[k]; m++) {
            len = snprintf(line, sizeof(line), "  sample");
            for (uint32_t w = 0; w < LT_SAMPLE_WORDS; w++) {
                uint64_t v = g_sample[k][m][w];
                Dl_info info = {0};
                if (v > 4096 && dladdr((void *)(uintptr_t)v, &info) && info.dli_sname)
                    len += snprintf(line + len, sizeof(line) - (size_t)len, " %s+0x%llx", info.dli_sname,
                                    (unsigned long long)(v - (uintptr_t)info.dli_saddr));
                else
                    len += snprintf(line + len, sizeof(line) - (size_t)len, " 0x%llx", (unsigned long long)v);
            }
            line[len++] = '\n';
            write(fd, line, (size_t)len);
        }
    }
    close(fd);
}

static void lt_note_alloc(void *p, size_t size) {
    if (!p || g_inside || g_failed) return;
    g_inside = 1;
    void *frames[LT_DEPTH + LT_SKIP];
    int depth = backtrace(frames, LT_DEPTH + LT_SKIP) - LT_SKIP;
    if (depth < 0) depth = 0;
    int dump = 0;
    pthread_mutex_lock(&g_lock);
    if (!g_ready) lt_init();
    if (g_ready) {
        uint32_t sid = lt_stack_id(frames + LT_SKIP, depth);
        Lt_Live *l = sid == UINT32_MAX ? 0 : lt_slot((uintptr_t)p, 1);
        if (l && !l->ptr) {
            *l = (Lt_Live){ (uintptr_t)p, size, sid };
            g_stacks[sid].count++;
            g_stacks[sid].bytes += size;
        } else {
            g_dropped++;
        }
        if ((++g_calls & 1023) == 0) {
            double now = lt_now();
            if (now >= g_next && !g_dumping) {
                g_next = now + g_every;
                g_dumping = dump = 1;
                lt_snapshot(now);
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (dump) {
        lt_write_snapshot(); /* g_dumping keeps g_snap stable meanwhile */
        __atomic_store_n(&g_dumping, 0, __ATOMIC_RELEASE);
    }
    g_inside = 0;
}

static void lt_note_free(void *p) {
    if (!p || !g_ready || g_inside) return; /* frees from inside the tracer (qsort) */
    pthread_mutex_lock(&g_lock);
    Lt_Live *l = lt_slot((uintptr_t)p, 0);
    if (l) {
        g_stacks[l->stack].count--;
        g_stacks[l->stack].bytes -= l->size;
        lt_remove(l);
    }
    pthread_mutex_unlock(&g_lock);
}

void *malloc(size_t n) {
    void *p = __libc_malloc(n);
    lt_note_alloc(p, n);
    return p;
}

void *calloc(size_t a, size_t b) {
    void *p = __libc_calloc(a, b);
    lt_note_alloc(p, a * b);
    return p;
}

void *realloc(void *old, size_t n) {
    if (!old) return malloc(n);
    void *p = __libc_realloc(old, n);
    if (!p) return 0;
    lt_note_free(old);
    lt_note_alloc(p, n);
    return p;
}

void free(void *p) {
    lt_note_free(p);
    __libc_free(p);
}

void *memalign(size_t align, size_t n) {
    void *p = __libc_memalign(align, n);
    lt_note_alloc(p, n);
    return p;
}

void *aligned_alloc(size_t align, size_t n) { return memalign(align, n); }

int posix_memalign(void **out, size_t align, size_t n) {
    void *p = memalign(align, n);
    if (!p) return 12; /* ENOMEM */
    *out = p;
    return 0;
}

void *valloc(size_t n) { return memalign((size_t)sysconf(_SC_PAGESIZE), n); }
