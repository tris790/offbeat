/*
 * Album art: thumbnail atlas with LRU slots, background decode workers with a
 * disk cache, now-playing artwork with palette extraction. See covers.h.
 *
 * Threading:
 *  - UI thread owns the entry table, the atlas slots and all GPU calls
 *    (covers_thumb / covers_update / covers_set_art).
 *  - Workers only see copied jobs; they hand pixels back through a bounded
 *    done-queue (fixed pool of thumbnail buffers) and one art slot.
 *  - One mutex guards the shared queues; nothing slow happens under it.
 *
 * Disk cache (<cache_dir>/thumbs):
 *    <key>.jpg      64x64 thumbnail; a zero-byte file means "no art"
 *    <key>_512.jpg  now-playing art
 *  key = mix(path_hash, file_size, mtime), so edited files get new art.
 *
 * Memory: full-size decodes are gated by a byte budget (estimated from the
 * image header before decoding), so concurrent workers can't stack up large
 * transient buffers. Everything goes through the tracked heap.
 */

#include "covers.h"
#include "../core/image.h"
#include "../core/hash.h"
#include "../platform/platform.h"
#include "../third_party/stb_image.h"
#include "../third_party/stb_image_write.h"

#include <stdio.h>
#include <string.h>

#define COV_ATLAS_SIZE        1024
#define COV_ATLAS_GRID        (COV_ATLAS_SIZE / COVER_THUMB_SIZE)   /* 16  */
#define COV_SLOTS             (COV_ATLAS_GRID * COV_ATLAS_GRID)     /* 256 */
#define COV_ENTRY_CAP         512    /* resident + pending + known-missing  */
#define COV_MAP_CAP           1024   /* power of two, >= 2 * entries        */
#define COV_REQ_CAP           64     /* LIFO request stack                  */
#define COV_POOL_CAP          32     /* thumbnail pixel buffers in flight   */
#define COV_DONE_CAP          (COV_POOL_CAP * 2)
#define COV_PATH_MAX          1024
#define COV_UPLOADS_PER_FRAME 24
#define COV_STALE_SECONDS     0.5
#define COV_MAX_WORKERS       4
#define COV_STREAM_BUF        (32 * 1024)
#define COV_DECODE_BUDGET     CORE_MB(16)  /* concurrent full-size decode bytes */
#define COV_MAX_PIXELS        (8192u * 8192u)
#define COV_THUMB_BYTES       (COVER_THUMB_SIZE * COVER_THUMB_SIZE * 4)
#define COV_ART_BYTES         (COVER_ART_SIZE * COVER_ART_SIZE * 4)

enum { COV_JOB_NONE = 0, COV_JOB_THUMB, COV_JOB_ART, COV_JOB_PREFETCH };
enum { COV_FREE = 0, COV_PENDING, COV_READY, COV_MISSING };
enum { COV_OK = 0, COV_NO_ART, COV_FAILED };

typedef struct {
    u64  hash;               /* track path hash (identity)   */
    u64  key;                /* disk cache key               */
    u32  cover_offset;
    u32  cover_size;
    u32  kind;               /* COV_JOB_*                    */
    char path[COV_PATH_MAX];
} Cov_Job;

typedef struct {
    u64 hash;
    u64 last_frame;          /* frame of the last covers_thumb for it */
    f64 last_time;
    f32 age;
    u16 slot;                /* atlas slot while READY */
    u8  state;               /* COV_FREE/PENDING/READY/MISSING */
} Cov_Entry;

typedef struct {
    u64 hash;
    b32 missing;
    u32 pool_index;          /* valid when !missing */
} Cov_Done;

typedef struct {
    u64 hash, key;
    u32 cover_offset, cover_size;
    u32 path_off, path_len;  /* into the prefetch path blob */
} Cov_PrefetchItem;

typedef struct Cov_Worker {
    struct Covers   *c;
    Platform_Thread *thread;
    Cov_Job          job;
    u8               thumb[COV_THUMB_BYTES];
    u8               stream_buf[COV_STREAM_BUF];
    /* resampler scratch (sized for the largest output) */
    _Alignas(f32) u8 image_scratch[COVER_ART_SIZE * CORE_IMAGE_SCRATCH_PER_PIXEL];
} Cov_Worker;

struct Covers {
    Core_Renderer *r;
    Core_ImageColorTables color_tables;
    char           thumbs_dir[COV_PATH_MAX];
    Core_Texture   atlas;
    Core_Texture   art_tex[2];

    /* ---- UI thread only ---- */
    Cover_Art  art[2];
    u32        art_cur;
    Cov_Entry  entries[COV_ENTRY_CAP];
    u16        map[COV_MAP_CAP];           /* entry index + 1, linear probing */
    u16        free_entries[COV_ENTRY_CAP];
    u32        free_entry_count;
    u16        free_slots[COV_SLOTS];
    u32        free_slot_count;
    u32        resident;
    u64        frame;
    f64        time;

    /* ---- shared, guarded by mutex ---- */
    Platform_Mutex mutex;
    Platform_Cond  work_cond;    /* new jobs / quit              */
    Platform_Cond  space_cond;   /* done-queue space / budget    */
    b32            quit;
    Cov_Job        req[COV_REQ_CAP];   /* LIFO: top = newest       */
    u32            req_count;
    Cov_Job        art_req;
    b32            art_req_pending;
    Cov_Done       done[COV_DONE_CAP]; /* FIFO ring                */
    u32            done_head, done_count;
    u8            *pool;               /* COV_POOL_CAP thumbnails  */
    u32            pool_free[COV_POOL_CAP];
    u32            pool_free_count;
    u8            *art_done_pixels;    /* 0 = none                 */
    u64            art_done_hash;
    b32            art_done_valid, art_done_missing;
    vec3           art_done_palette[4];
    u64            decode_in_use;      /* bytes reserved by full decodes */
    Cov_PrefetchItem *pf_items;        /* one heap block: items + paths */
    u32            pf_count, pf_next;
    b32            pf_active;
    u32            in_flight;          /* thumb/art jobs being worked on */

    /* stats (guarded by mutex) */
    u64 stat_generated, stat_cache_hits, stat_missing;
    f64 stat_generate_s, stat_cache_hit_s;

    Platform_Mutex write_mutex;  /* platform_file_write_all uses one temp name per process */
    Cov_Worker    *workers;
    u32            worker_count;
};

/* ---- keys & paths ---- */



static u64 cov_key(const Lib_Track *t) {
    return core_mix_u64(t->path_hash ^ core_mix_u64(t->file_size + 0x9E3779B97F4A7C15ull) ^
                   core_mix_u64((u64)t->mtime_ns * 0x2545F4914F6CDD1Dull));
}

static void cov_cache_path(Covers *c, char *out, u64 key, const char *suffix) {
    snprintf(out, COV_PATH_MAX + 64, "%s/%016llx%s.jpg", c->thumbs_dir, (unsigned long long)key, suffix);
}

static b32 cov_job_from_track(Cov_Job *j, const Lib_Track *t, u32 kind) {
    if (t->path.len >= COV_PATH_MAX) return false;
    j->hash = t->path_hash;
    j->key  = cov_key(t);
    j->cover_offset = t->cover_size ? t->cover_offset : 0;
    j->cover_size   = t->cover_size;
    j->kind = kind;
    memcpy(j->path, t->path.str, t->path.len);
    j->path[t->path.len] = 0;
    return true;
}

/* ---- streaming decode input ----
   stb_image pulls bytes through callbacks from [start, end) of a file, so the
   encoded picture is never copied whole into memory. */

typedef struct {
    Platform_File f;
    u64 start, end, pos;     /* pos = file offset of buf[buf_len] */
    u8 *buf;
    u32 buf_len, buf_pos;
} Cov_Stream;

static int cov_cb_read(void *user, char *data, int size) {
    Cov_Stream *s = user;
    int done = 0;
    while (done < size) {
        if (s->buf_pos == s->buf_len) {
            u64 left = s->end - s->pos;
            if (!left) break;
            if ((u64)(size - done) >= COV_STREAM_BUF) { /* big read: straight into stb's buffer */
                u64 want = CORE_MIN(left, (u64)(size - done));
                u64 got = platform_file_read_at(s->f, s->pos, data + done, want);
                s->pos += got;
                done += (int)got;
                if (got < want) break;
                continue;
            }
            u64 got = platform_file_read_at(s->f, s->pos, s->buf, CORE_MIN(left, (u64)COV_STREAM_BUF));
            if (!got) break;
            s->pos += got;
            s->buf_len = (u32)got;
            s->buf_pos = 0;
        }
        u32 n = CORE_MIN(s->buf_len - s->buf_pos, (u32)(size - done));
        memcpy(data + done, s->buf + s->buf_pos, n);
        s->buf_pos += n;
        done += (int)n;
    }
    return done;
}

static void cov_cb_skip(void *user, int n) {
    Cov_Stream *s = user;
    s64 np = (s64)s->buf_pos + n;
    if (np >= 0 && np <= (s64)s->buf_len) { s->buf_pos = (u32)np; return; }
    s64 at = (s64)(s->pos - (s->buf_len - s->buf_pos)) + n; /* logical position */
    s->pos = (u64)CORE_CLAMP(at, (s64)s->start, (s64)s->end);
    s->buf_len = s->buf_pos = 0;
}

static int cov_cb_eof(void *user) {
    Cov_Stream *s = user;
    return s->buf_pos == s->buf_len && s->pos >= s->end;
}

static const stbi_io_callbacks g_cov_io = { cov_cb_read, cov_cb_skip, cov_cb_eof };

static void cov_stream_rewind(Cov_Stream *s) { s->pos = s->start; s->buf_len = s->buf_pos = 0; }

/* ---- decode budget ---- */

static void cov_budget_acquire(Covers *c, u64 bytes) {
    platform_mutex_lock(&c->mutex);
    /* A single decode larger than the whole budget still runs, alone. */
    while (!c->quit && c->decode_in_use && c->decode_in_use + bytes > COV_DECODE_BUDGET)
        platform_cond_wait(&c->space_cond, &c->mutex);
    c->decode_in_use += bytes;
    platform_mutex_unlock(&c->mutex);
}

static void cov_budget_release(Covers *c, u64 bytes) {
    platform_mutex_lock(&c->mutex);
    c->decode_in_use -= bytes;
    platform_cond_broadcast(&c->space_cond);
    platform_mutex_unlock(&c->mutex);
}

/* Decode [start, start+size) of `path` (size 0 = whole file). Returns stb
   pixels (native channel count) or 0; *reserved must be released by the
   caller after freeing the pixels. */
static u8 *cov_decode_file(Covers *c, Cov_Worker *w, const char *path, u64 start, u64 size,
                           int *iw, int *ih, int *ich, u64 *reserved) {
    *reserved = 0;
    Cov_Stream s = { .f = platform_file_open_read(path), .buf = w->stream_buf };
    if (!platform_file_valid(s.f)) return 0;
    u64 fsize = platform_file_size(s.f);
    if (!size) size = fsize;
    if (start >= fsize || size > fsize - start) { platform_file_close(s.f); return 0; }
    s.start = start;
    s.end = start + size;
    cov_stream_rewind(&s);

    u8 *pixels = 0;
    int x, y, comp;
    if (stbi_info_from_callbacks(&g_cov_io, &s, &x, &y, &comp) && x > 0 && y > 0 &&
        (u64)x * (u64)y <= COV_MAX_PIXELS) {
        /* stb peak ~ two full-size buffers (+ chroma planes / IDAT copy). */
        u64 est = (u64)x * y * (u64)CORE_MAX(comp, 3) * 2 + (u64)x * y + size;
        cov_budget_acquire(c, est);
        *reserved = est;
        cov_stream_rewind(&s);
        pixels = stbi_load_from_callbacks(&g_cov_io, &s, iw, ih, ich, 0);
        if (!pixels) { cov_budget_release(c, est); *reserved = 0; }
    }
    platform_file_close(s.f);
    return pixels;
}

/* ---- palette ---- */


/* Deterministic per-track fallback palette (no art). */
static void cov_default_palette(u64 hash, vec3 out[4]) {
    f32 h = (f32)(core_mix_u64(hash) % 360) / 360.0f;
    out[0] = core_hsv_to_rgb(h, 0.35f, 0.10f);
    out[1] = core_hsv_to_rgb(h, 0.30f, 0.22f);
    out[2] = core_hsv_to_rgb(h, 0.25f, 0.45f);
    out[3] = core_hsv_to_rgb(h, 0.60f, 0.95f);
}

/* k-means (k = 5, farthest-point init) on a 32x32 downsample.
   out[0] = darkest of the dominant clusters, out[1..2] = the next biggest by
   luminance, out[3] = most vibrant (saturation * value), boosted to work as
   an accent on a dark UI. */
static void cov_palette(const u8 *rgba, u32 size, vec3 out[4]) {
    enum { N = 32, K = 5, SAMPLES = N * N };
    static const u32 iterations = 10;
    vec3 px[SAMPLES];
    u32 block = size / N;
    for (u32 by = 0; by < N; by++) {
        for (u32 bx = 0; bx < N; bx++) {
            u32 r = 0, g = 0, b = 0;
            for (u32 y = 0; y < block; y++) {
                const u8 *p = rgba + (((u64)by * block + y) * size + (u64)bx * block) * 4;
                for (u32 x = 0; x < block; x++, p += 4) { r += p[0]; g += p[1]; b += p[2]; }
            }
            f32 inv = 1.0f / (255.0f * block * block);
            px[by * N + bx] = vec3_make(r * inv, g * inv, b * inv);
        }
    }

    vec3 mean = vec3_make(0, 0, 0);
    for (u32 i = 0; i < SAMPLES; i++) mean = vec3_add(mean, px[i]);
    mean = vec3_mul(mean, 1.0f / SAMPLES);

    vec3 ctr[K];
    u32 count[K];
    ctr[0] = mean;
    for (u32 k = 1; k < K; k++) {
        f32 best = -1;
        u32 bi = 0;
        for (u32 i = 0; i < SAMPLES; i++) {
            f32 dmin = 1e9f;
            for (u32 j = 0; j < k; j++) {
                vec3 d = vec3_sub(px[i], ctr[j]);
                dmin = CORE_MIN(dmin, vec3_dot(d, d));
            }
            if (dmin > best) { best = dmin; bi = i; }
        }
        ctr[k] = px[bi];
    }
    for (u32 it = 0; it < iterations; it++) {
        vec3 sum[K];
        for (u32 k = 0; k < K; k++) { sum[k] = vec3_make(0, 0, 0); count[k] = 0; }
        for (u32 i = 0; i < SAMPLES; i++) {
            u32 bk = 0;
            f32 bd = 1e9f;
            for (u32 k = 0; k < K; k++) {
                vec3 d = vec3_sub(px[i], ctr[k]);
                f32 dd = vec3_dot(d, d);
                if (dd < bd) { bd = dd; bk = k; }
            }
            sum[bk] = vec3_add(sum[bk], px[i]);
            count[bk]++;
        }
        for (u32 k = 0; k < K; k++) if (count[k]) ctr[k] = vec3_mul(sum[k], 1.0f / count[k]);
    }

    /* [0]: darkest cluster holding >= 15% of pixels (or the biggest). */
    u32 biggest = 0;
    for (u32 k = 1; k < K; k++) if (count[k] > count[biggest]) biggest = k;
    u32 dark = biggest;
    for (u32 k = 0; k < K; k++)
        if (count[k] >= SAMPLES * 15 / 100 && core_rgb_luma(ctr[k]) < core_rgb_luma(ctr[dark])) dark = k;

    /* [3]: most vibrant; tiny clusters are discounted so specks don't win. */
    u32 vib = dark == 0 ? 1 : 0;
    f32 best = -1;
    for (u32 k = 0; k < K; k++) {
        if (k == dark || !count[k]) continue;
        vec3 hsv = core_rgb_to_hsv(ctr[k]);
        f32 score = hsv.y * hsv.z * (count[k] >= SAMPLES / 50 ? 1.0f : 0.3f);
        if (score > best) { best = score; vib = k; }
    }

    /* [1], [2]: the two biggest remaining, ordered dark -> light. */
    u32 rest[2] = { dark, dark };
    u32 rc = 0;
    for (u32 pass = 0; pass < 2; pass++) {
        u32 pick = K;
        for (u32 k = 0; k < K; k++) {
            if (k == dark || k == vib || (rc > 0 && k == rest[0]) || !count[k]) continue;
            if (pick == K || count[k] > count[pick]) pick = k;
        }
        if (pick < K) rest[rc++] = pick;
    }
    out[0] = ctr[dark];
    vec3 m1 = rc > 0 ? ctr[rest[0]] : vec3_lerp(ctr[dark], ctr[vib], 0.33f);
    vec3 m2 = rc > 1 ? ctr[rest[1]] : vec3_lerp(ctr[dark], ctr[vib], 0.66f);
    if (core_rgb_luma(m1) > core_rgb_luma(m2)) { vec3 t = m1; m1 = m2; m2 = t; }
    out[1] = m1;
    out[2] = m2;

    vec3 hsv = core_rgb_to_hsv(ctr[vib]);
    f32 h = hsv.x, s = hsv.y, v = hsv.z;
    if (s >= 0.12f) s = CORE_MAX(s, 0.55f); /* keep grayscale art gray */
    v = CORE_MAX(v, 0.85f);
    out[3] = core_hsv_to_rgb(h, CORE_MIN(s, 1.0f), CORE_MIN(v, 1.0f));
}

/* ---- disk cache ---- */

typedef struct { u8 *data; u64 len, cap; b32 failed; } Cov_Mem;

static void cov_mem_write(void *ctx, void *data, int size) {
    Cov_Mem *m = ctx;
    if (m->failed || size <= 0) return;
    if (m->len + (u64)size > m->cap) {
        u64 cap = CORE_MAX(m->cap * 2, m->len + (u64)size);
        u8 *p = core_heap_realloc(m->data, cap);
        if (!p) { m->failed = true; return; }
        m->data = p;
        m->cap = cap;
    }
    memcpy(m->data + m->len, data, (u64)size);
    m->len += (u64)size;
}

static void cov_write_file(Covers *c, const char *path, const void *data, u64 size) {
    platform_mutex_lock(&c->write_mutex);
    platform_file_write_all(path, data, size);
    platform_mutex_unlock(&c->write_mutex);
}

static void cov_write_jpg(Covers *c, const char *path, const u8 *rgba, u32 size) {
    Cov_Mem m = { .cap = (u64)size * size / 2 + 4096 };
    m.data = core_heap_alloc(m.cap);
    if (!m.data) return;
    if (stbi_write_jpg_to_func(cov_mem_write, &m, (int)size, (int)size, 4, rgba, 90) && !m.failed)
        cov_write_file(c, path, m.data, m.len);
    core_heap_free(m.data);
}

/* Load a cached square jpg of exactly `size`. */
static b32 cov_load_cached(const char *path, u64 file_size, u8 *out, u32 size) {
    if (file_size == 0 || file_size > CORE_MB(4)) return false;
    Platform_File f = platform_file_open_read(path);
    if (!platform_file_valid(f)) return false;
    u8 *buf = core_heap_alloc(file_size);
    b32 ok = false;
    if (buf && platform_file_read_at(f, 0, buf, file_size) == file_size) {
        int w, h, ch;
        u8 *px = stbi_load_from_memory(buf, (int)file_size, &w, &h, &ch, 4);
        if (px && (u32)w == size && (u32)h == size) { memcpy(out, px, (u64)size * size * 4); ok = true; }
        if (px) stbi_image_free(px);
    }
    core_heap_free(buf);
    platform_file_close(f);
    return ok;
}

/* ---- producing pixels ---- */

/* Decode the track's picture (embedded, else cover/folder image next to it)
   and resample it to `size`. */
static u32 cov_generate(Covers *c, Cov_Worker *w, const Cov_Job *job, u8 *out, u32 size) {
    int iw = 0, ih = 0, ich = 0;
    u64 reserved = 0;
    u8 *img = 0;
    if (job->cover_size) {
        img = cov_decode_file(c, w, job->path, job->cover_offset, job->cover_size, &iw, &ih, &ich, &reserved);
    } else {
        static const char *const names[] = {
            "cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg", "Cover.jpg", "Folder.jpg", "AlbumArt.jpg",
        };
        char dir[COV_PATH_MAX + 32];
        u64 len = strlen(job->path);
        while (len > 0 && job->path[len - 1] != '/') len--;
        for (u32 i = 0; i < CORE_ARRAY_COUNT(names) && !img; i++) {
            snprintf(dir, sizeof(dir), "%.*s%s", (int)len, job->path, names[i]);
            if (platform_file_info(dir).exists)
                img = cov_decode_file(c, w, dir, 0, 0, &iw, &ih, &ich, &reserved);
        }
    }
    if (!img) return COV_NO_ART;
    core_image_resize_square(&w->c->color_tables, img, (u32)iw, (u32)ih, (u32)ich, out, size, w->image_scratch, COVER_ART_SIZE);
    stbi_image_free(img);
    cov_budget_release(c, reserved);
    return COV_OK;
}

/* Thumbnail from the disk cache, else generated (and cached). */
static u32 cov_produce_thumb(Covers *c, Cov_Worker *w, const Cov_Job *job, u8 *out) {
    char path[COV_PATH_MAX + 64];
    cov_cache_path(c, path, job->key, "");
    f64 t0 = platform_time_seconds();
    Platform_FileInfo fi = platform_file_info(path);
    if (fi.exists && fi.size == 0) return COV_NO_ART;
    if (fi.exists && cov_load_cached(path, fi.size, out, COVER_THUMB_SIZE)) {
        platform_mutex_lock(&c->mutex);
        c->stat_cache_hits++;
        c->stat_cache_hit_s += platform_time_seconds() - t0;
        platform_mutex_unlock(&c->mutex);
        return COV_OK;
    }
    if (!platform_file_info(job->path).exists) return COV_FAILED; /* don't cache */
    u32 r = cov_generate(c, w, job, out, COVER_THUMB_SIZE);
    if (r == COV_OK) cov_write_jpg(c, path, out, COVER_THUMB_SIZE);
    else             cov_write_file(c, path, "", 0);            /* "no art" marker */
    platform_mutex_lock(&c->mutex);
    if (r == COV_OK) { c->stat_generated++; c->stat_generate_s += platform_time_seconds() - t0; }
    else c->stat_missing++;
    platform_mutex_unlock(&c->mutex);
    return r;
}

static void cov_deliver_thumb(Covers *c, u64 hash, b32 missing, const u8 *px) {
    platform_mutex_lock(&c->mutex);
    while (!c->quit && (c->done_count == COV_DONE_CAP || (!missing && c->pool_free_count == 0)))
        platform_cond_wait(&c->space_cond, &c->mutex);
    if (!c->quit) {
        Cov_Done *d = &c->done[(c->done_head + c->done_count++) % COV_DONE_CAP];
        d->hash = hash;
        d->missing = missing;
        if (!missing) {
            d->pool_index = c->pool_free[--c->pool_free_count];
            memcpy(c->pool + (u64)d->pool_index * COV_THUMB_BYTES, px, COV_THUMB_BYTES);
        }
    }
    platform_mutex_unlock(&c->mutex);
}

static void cov_run_art(Covers *c, Cov_Worker *w, const Cov_Job *job) {
    u8 *px = core_heap_alloc(COV_ART_BYTES);
    b32 missing = true;
    char path[COV_PATH_MAX + 64], thumb_path[COV_PATH_MAX + 64];
    cov_cache_path(c, path, job->key, "_512");
    cov_cache_path(c, thumb_path, job->key, "");
    if (px) {
        Platform_FileInfo fi = platform_file_info(path);
        Platform_FileInfo ti = platform_file_info(thumb_path);
        if (fi.exists && cov_load_cached(path, fi.size, px, COVER_ART_SIZE)) {
            missing = false;
        } else if (!(ti.exists && ti.size == 0)) {
            u32 r = cov_generate(c, w, job, px, COVER_ART_SIZE);
            missing = r != COV_OK;
            if (!missing) {
                cov_write_jpg(c, path, px, COVER_ART_SIZE);
                if (!ti.exists) { /* the thumbnail is a free 8x8 box filter away */
                    core_image_resize_square(&w->c->color_tables, px, COVER_ART_SIZE, COVER_ART_SIZE, 4, w->thumb, COVER_THUMB_SIZE, w->image_scratch, COVER_ART_SIZE);
                    cov_write_jpg(c, thumb_path, w->thumb, COVER_THUMB_SIZE);
                }
            } else if (r == COV_NO_ART && platform_file_info(job->path).exists) {
                cov_write_file(c, thumb_path, "", 0);
            }
        }
    }
    vec3 pal[4];
    if (missing) {
        cov_default_palette(job->hash, pal);
        core_heap_free(px);
        px = 0;
    } else {
        cov_palette(px, COVER_ART_SIZE, pal);
    }

    platform_mutex_lock(&c->mutex);
    if (c->art_done_pixels) core_heap_free(c->art_done_pixels); /* superseded, never shown */
    c->art_done_pixels  = px;
    c->art_done_hash    = job->hash;
    c->art_done_missing = missing;
    c->art_done_valid   = true;
    memcpy(c->art_done_palette, pal, sizeof(pal));
    platform_mutex_unlock(&c->mutex);
}

static void cov_worker_main(void *user) {
    Cov_Worker *w = user;
    Covers *c = w->c;
    platform_thread_set_background();
    for (;;) {
        u32 kind = COV_JOB_NONE;
        platform_mutex_lock(&c->mutex);
        while (!c->quit) {
            if (c->art_req_pending) {
                w->job = c->art_req;
                c->art_req_pending = false;
                kind = COV_JOB_ART;
            } else if (c->req_count) {
                w->job = c->req[--c->req_count];  /* newest first */
                kind = COV_JOB_THUMB;
            } else if (!c->pf_active && c->pf_next < c->pf_count) {
                const Cov_PrefetchItem *it = &c->pf_items[c->pf_next++];
                const char *paths = (const char *)(c->pf_items + c->pf_count);
                w->job.hash = it->hash;
                w->job.key = it->key;
                w->job.cover_offset = it->cover_offset;
                w->job.cover_size = it->cover_size;
                memcpy(w->job.path, paths + it->path_off, it->path_len + 1);
                c->pf_active = true;
                kind = COV_JOB_PREFETCH;
            }
            if (kind) break;
            platform_cond_wait(&c->work_cond, &c->mutex);
        }
        if (kind != COV_JOB_PREFETCH && kind) c->in_flight++;
        platform_mutex_unlock(&c->mutex);
        if (!kind) break;

        if (kind == COV_JOB_ART) {
            cov_run_art(c, w, &w->job);
        } else if (kind == COV_JOB_THUMB) {
            u32 r = cov_produce_thumb(c, w, &w->job, w->thumb);
            cov_deliver_thumb(c, w->job.hash, r != COV_OK, w->thumb);
        } else {
            char path[COV_PATH_MAX + 64];
            cov_cache_path(c, path, w->job.key, "");
            if (!platform_file_info(path).exists) cov_produce_thumb(c, w, &w->job, w->thumb);
        }

        platform_mutex_lock(&c->mutex);
        if (kind == COV_JOB_PREFETCH) c->pf_active = false;
        else c->in_flight--;
        platform_mutex_unlock(&c->mutex);
    }
}

/* ---- entry table (UI thread) ---- */

static s32 cov_find(Covers *c, u64 hash) {
    for (u32 i = (u32)hash & (COV_MAP_CAP - 1);; i = (i + 1) & (COV_MAP_CAP - 1)) {
        u32 e = c->map[i];
        if (!e) return -1;
        if (c->entries[e - 1].hash == hash) return (s32)(e - 1);
    }
}

static void cov_map_insert(Covers *c, u32 entry) {
    u32 i = (u32)c->entries[entry].hash & (COV_MAP_CAP - 1);
    while (c->map[i]) i = (i + 1) & (COV_MAP_CAP - 1);
    c->map[i] = (u16)(entry + 1);
}

/* Remove + backward-shift so probe chains stay intact without tombstones. */
static void cov_map_remove(Covers *c, u32 entry) {
    const u32 mask = COV_MAP_CAP - 1;
    u32 i = (u32)c->entries[entry].hash & mask;
    while (c->map[i] != entry + 1) { if (!c->map[i]) return; i = (i + 1) & mask; }
    c->map[i] = 0;
    for (u32 j = (i + 1) & mask; c->map[j]; j = (j + 1) & mask) {
        u32 home = (u32)c->entries[c->map[j] - 1].hash & mask;
        if (((i - home) & mask) < ((j - home) & mask)) { c->map[i] = c->map[j]; c->map[j] = 0; i = j; }
    }
}

static void cov_entry_free(Covers *c, u32 e) {
    Cov_Entry *en = &c->entries[e];
    if (en->state == COV_FREE) return;
    cov_map_remove(c, e);
    if (en->state == COV_READY) { c->free_slots[c->free_slot_count++] = en->slot; c->resident--; }
    en->state = COV_FREE;
    c->free_entries[c->free_entry_count++] = (u16)e;
}

/* Least recently requested entry not used this frame (optionally READY
   only), or -1. */
static s32 cov_lru(Covers *c, b32 ready_only) {
    s32 best = -1;
    for (u32 i = 0; i < COV_ENTRY_CAP; i++) {
        Cov_Entry *en = &c->entries[i];
        if (en->state == COV_FREE || en->state == COV_PENDING || en->last_frame == c->frame) continue;
        if (ready_only && en->state != COV_READY) continue;
        if (best < 0 || en->last_time < c->entries[best].last_time) best = (s32)i;
    }
    return best;
}

static s32 cov_entry_new(Covers *c, u64 hash) {
    if (!c->free_entry_count) {
        s32 victim = cov_lru(c, false);
        if (victim < 0) return -1;
        cov_entry_free(c, (u32)victim);
    }
    u32 e = c->free_entries[--c->free_entry_count];
    c->entries[e] = (Cov_Entry){ .hash = hash, .state = COV_PENDING, .last_time = c->time };
    cov_map_insert(c, e);
    return (s32)e;
}

static s32 cov_slot_new(Covers *c) {
    if (!c->free_slot_count) {
        s32 victim = cov_lru(c, true);
        if (victim < 0) return -1;
        cov_entry_free(c, (u32)victim);
    }
    return c->free_slots[--c->free_slot_count];
}

/* ---- public API ---- */

Covers *covers_create(Core_Renderer *r, const char *cache_dir, u32 worker_count) {
    if (worker_count == 0) worker_count = 2;
    worker_count = CORE_MIN(worker_count, (u32)COV_MAX_WORKERS);

    Covers *c = core_heap_calloc(sizeof(Covers));
    if (!c) return 0;
    core_image_color_tables_init(&c->color_tables);
    c->r = r;
    snprintf(c->thumbs_dir, sizeof(c->thumbs_dir), "%s/thumbs", cache_dir);
    platform_make_dirs(c->thumbs_dir);

    c->pool = core_heap_alloc((u64)COV_POOL_CAP * COV_THUMB_BYTES);
    c->workers = core_heap_calloc(sizeof(Cov_Worker) * COV_MAX_WORKERS);
    if (!c->pool || !c->workers) {
        core_heap_free(c->pool);
        core_heap_free(c->workers);
        core_heap_free(c);
        return 0;
    }
    for (u32 i = 0; i < COV_POOL_CAP; i++) c->pool_free[i] = i;
    c->pool_free_count = COV_POOL_CAP;
    for (u32 i = 0; i < COV_ENTRY_CAP; i++) c->free_entries[i] = (u16)(COV_ENTRY_CAP - 1 - i);
    c->free_entry_count = COV_ENTRY_CAP;
    for (u32 i = 0; i < COV_SLOTS; i++) c->free_slots[i] = (u16)(COV_SLOTS - 1 - i);
    c->free_slot_count = COV_SLOTS;

    c->atlas = core_texture_create(r, CORE_TEXTURE_RGBA8, COV_ATLAS_SIZE, COV_ATLAS_SIZE, 0, false);
    for (u32 i = 0; i < 2; i++) {
        c->art_tex[i] = core_texture_create(r, CORE_TEXTURE_RGBA8, COVER_ART_SIZE, COVER_ART_SIZE, 0, true);
        c->art[i].tex = c->art_tex[i];
        cov_default_palette(0, c->art[i].palette);
    }

    platform_mutex_init(&c->mutex);
    platform_mutex_init(&c->write_mutex);
    platform_cond_init(&c->work_cond);
    platform_cond_init(&c->space_cond);
    for (u32 i = 0; i < worker_count; i++) {
        c->workers[i].c = c;
        c->workers[i].thread = platform_thread_start(cov_worker_main, &c->workers[i], "covers");
        if (c->workers[i].thread) c->worker_count++;
    }
    return c;
}

void covers_destroy(Covers *c) {
    if (!c) return;
    platform_mutex_lock(&c->mutex);
    c->quit = true;
    platform_cond_broadcast(&c->work_cond);
    platform_cond_broadcast(&c->space_cond);
    platform_mutex_unlock(&c->mutex);
    for (u32 i = 0; i < COV_MAX_WORKERS && c->workers; i++)
        if (c->workers[i].thread) platform_thread_join(c->workers[i].thread);
    core_texture_destroy(c->r, c->atlas);
    core_texture_destroy(c->r, c->art_tex[0]);
    core_texture_destroy(c->r, c->art_tex[1]);
    core_heap_free(c->art_done_pixels);
    core_heap_free(c->pf_items);
    core_heap_free(c->pool);
    core_heap_free(c->workers);
    core_heap_free(c);
}

static void cov_request(Covers *c, const Lib_Track *t) {
    Cov_Job *job;
    u64 dropped = 0;
    b32 have_drop = false;
    platform_mutex_lock(&c->mutex);
    if (c->req_count == COV_REQ_CAP) {        /* full: forget the oldest */
        dropped = c->req[0].hash;
        have_drop = true;
        memmove(c->req, c->req + 1, sizeof(Cov_Job) * (COV_REQ_CAP - 1));
        c->req_count--;
    }
    job = &c->req[c->req_count];
    if (cov_job_from_track(job, t, COV_JOB_THUMB)) {
        c->req_count++;
        platform_cond_signal(&c->work_cond);
    }
    platform_mutex_unlock(&c->mutex);
    if (have_drop) {
        s32 e = cov_find(c, dropped);
        if (e >= 0 && c->entries[e].state == COV_PENDING) cov_entry_free(c, (u32)e);
    }
}

Cover_Thumb covers_thumb(Covers *c, const Lib_Track *t) {
    Cover_Thumb out = {0};
    if (!c || !t) return out;
    out.tex = c->atlas;
    s32 e = cov_find(c, t->path_hash);
    if (e < 0) {
        e = cov_entry_new(c, t->path_hash);
        if (e < 0) return out;                /* every entry is in use this frame */
        if (t->path.len >= COV_PATH_MAX) c->entries[e].state = COV_MISSING;
        else cov_request(c, t);
        e = cov_find(c, t->path_hash);        /* the request may have evicted */
        if (e < 0) return out;
    }
    Cov_Entry *en = &c->entries[e];
    en->last_frame = c->frame;
    en->last_time = c->time;
    if (en->state == COV_READY) {
        const f32 inv = 1.0f / COV_ATLAS_SIZE;
        u32 sx = (en->slot % COV_ATLAS_GRID) * COVER_THUMB_SIZE;
        u32 sy = (en->slot / COV_ATLAS_GRID) * COVER_THUMB_SIZE;
        out.uv0 = vec2_make((sx + 0.5f) * inv, (sy + 0.5f) * inv);
        out.uv1 = vec2_make((sx + COVER_THUMB_SIZE - 0.5f) * inv, (sy + COVER_THUMB_SIZE - 0.5f) * inv);
        out.ready = true;
        out.age = en->age;
    } else if (en->state == COV_MISSING) {
        out.missing = true;
        out.age = en->age;
    }
    return out;
}

void covers_update(Covers *c, f32 dt) {
    if (!c) return;
    c->frame++;
    c->time += dt;
    for (u32 i = 0; i < COV_ENTRY_CAP; i++) {
        Cov_Entry *en = &c->entries[i];
        if ((en->state == COV_READY || en->state == COV_MISSING) && en->age < 1e6f) en->age += dt;
    }
    for (u32 i = 0; i < 2; i++)
        if ((c->art[i].ready || c->art[i].missing) && c->art[i].age < 1e6f) c->art[i].age += dt;

    Cov_Done got[COV_UPLOADS_PER_FRAME * 2];
    u32 got_count = 0;
    u64 stale[COV_REQ_CAP];
    u32 stale_count = 0;
    u8 *art_px = 0;
    u64 art_hash = 0;
    b32 art_valid = false, art_missing = false;
    vec3 art_pal[4];

    platform_mutex_lock(&c->mutex);
    /* Drop requests nobody asked for recently (fast scrolling). */
    u32 kept = 0;
    for (u32 i = 0; i < c->req_count; i++) {
        s32 e = cov_find(c, c->req[i].hash);
        if (e < 0 || c->time - c->entries[e].last_time > COV_STALE_SECONDS) {
            stale[stale_count++] = c->req[i].hash;
            continue;
        }
        if (kept != i) c->req[kept] = c->req[i];
        kept++;
    }
    c->req_count = kept;
    /* Finished thumbnails, bounded per frame (missing ones are free). */
    u32 uploads = 0;
    while (c->done_count && got_count < CORE_ARRAY_COUNT(got) && uploads < COV_UPLOADS_PER_FRAME) {
        Cov_Done d = c->done[c->done_head];
        c->done_head = (c->done_head + 1) % COV_DONE_CAP;
        c->done_count--;
        got[got_count++] = d;
        if (!d.missing) uploads++;
    }
    if (c->art_done_valid) {
        art_valid = true;
        art_px = c->art_done_pixels;
        art_hash = c->art_done_hash;
        art_missing = c->art_done_missing;
        memcpy(art_pal, c->art_done_palette, sizeof(art_pal));
        c->art_done_pixels = 0;
        c->art_done_valid = false;
    }
    platform_mutex_unlock(&c->mutex);

    for (u32 i = 0; i < stale_count; i++) {
        s32 e = cov_find(c, stale[i]);
        if (e >= 0 && c->entries[e].state == COV_PENDING) cov_entry_free(c, (u32)e);
    }

    for (u32 i = 0; i < got_count; i++) {
        Cov_Done *d = &got[i];
        s32 e = cov_find(c, d->hash);
        if (e < 0) e = cov_entry_new(c, d->hash); /* dropped meanwhile: keep the work */
        if (e < 0) continue;
        Cov_Entry *en = &c->entries[e];
        if (en->state != COV_PENDING) continue;   /* duplicate result */
        if (d->missing) { en->state = COV_MISSING; en->age = 0; continue; }
        s32 slot = cov_slot_new(c);            /* only evicts READY entries */
        if (slot < 0) { cov_entry_free(c, (u32)e); continue; }
        core_texture_update(c->r, c->atlas, (slot % COV_ATLAS_GRID) * COVER_THUMB_SIZE,
                            (slot / COV_ATLAS_GRID) * COVER_THUMB_SIZE, COVER_THUMB_SIZE, COVER_THUMB_SIZE,
                            c->pool + (u64)d->pool_index * COV_THUMB_BYTES);
        en->state = COV_READY;
        en->slot = (u16)slot;
        en->age = 0;
        c->resident++;
    }

    if (got_count) {
        platform_mutex_lock(&c->mutex);
        for (u32 i = 0; i < got_count; i++)
            if (!got[i].missing) c->pool_free[c->pool_free_count++] = got[i].pool_index;
        platform_cond_broadcast(&c->space_cond);
        platform_mutex_unlock(&c->mutex);
    }

    if (art_valid) {
        Cover_Art *a = &c->art[c->art_cur];
        if (art_hash == a->track_hash && !a->ready && !a->missing) {
            if (!art_missing && art_px) {
                core_texture_update(c->r, a->tex, 0, 0, COVER_ART_SIZE, COVER_ART_SIZE, art_px);
                core_texture_generate_mips(c->r, a->tex);
                a->ready = true;
            } else {
                a->missing = true;
            }
            a->age = 0;
            memcpy(a->palette, art_pal, sizeof(art_pal));
        }
        core_heap_free(art_px);
    }
}

void covers_set_art(Covers *c, const Lib_Track *t) {
    if (!c || !t) return;
    Cover_Art *cur = &c->art[c->art_cur];
    if (cur->track_hash == t->path_hash) return;
    /* A finished current art becomes "previous"; an unfinished one is simply
       replaced so the crossfade starts from the last art actually shown. */
    if (cur->ready || cur->missing) c->art_cur ^= 1;
    cur = &c->art[c->art_cur];
    *cur = (Cover_Art){ .tex = c->art_tex[c->art_cur], .track_hash = t->path_hash };
    cov_default_palette(t->path_hash, cur->palette);

    platform_mutex_lock(&c->mutex);
    if (cov_job_from_track(&c->art_req, t, COV_JOB_ART)) {
        c->art_req_pending = true;
        platform_cond_signal(&c->work_cond);
    } else {
        cur->missing = true;
    }
    platform_mutex_unlock(&c->mutex);
}

Cover_Art covers_art(Covers *c) {
    Cover_Art a = {0};
    return c ? c->art[c->art_cur] : a;
}

Cover_Art covers_art_prev(Covers *c) {
    Cover_Art a = {0};
    return c ? c->art[c->art_cur ^ 1] : a;
}

void covers_prefetch_library(Covers *c, const Library *lib) {
    if (!c) return;
    u32 n = lib ? lib->track_count : 0;
    u64 path_bytes = 0;
    for (u32 i = 0; i < n; i++) path_bytes += lib->tracks[i].path.len + 1;
    Cov_PrefetchItem *items = 0;
    u32 count = 0;
    if (n) {
        items = core_heap_alloc(sizeof(Cov_PrefetchItem) * n + path_bytes);
        if (!items) return;
        char *paths = (char *)(items + n);
        u64 at = 0;
        for (u32 k = 0; k < n; k++) {
            /* Title order: what a freshly opened list shows first. */
            const Lib_Track *t = &lib->tracks[lib->by_title ? lib->by_title[k] : k];
            if (t->path.len >= COV_PATH_MAX) continue;
            Cov_PrefetchItem *it = &items[count++];
            it->hash = t->path_hash;
            it->key = cov_key(t);
            it->cover_offset = t->cover_offset;
            it->cover_size = t->cover_size;
            it->path_off = (u32)at;
            it->path_len = (u32)t->path.len;
            memcpy(paths + at, t->path.str, t->path.len);
            paths[at + t->path.len] = 0;
            at += t->path.len + 1;
        }
        /* Paths must follow items[count]: compact if some were skipped. */
        if (count < n) memmove(items + count, paths, at);
    }
    platform_mutex_lock(&c->mutex);
    Cov_PrefetchItem *old = c->pf_items;
    c->pf_items = items;
    c->pf_count = count;
    c->pf_next = 0;
    platform_cond_signal(&c->work_cond);
    platform_mutex_unlock(&c->mutex);
    core_heap_free(old); /* workers copy an item under the lock, so this is safe */
}

Covers_Stats covers_stats(Covers *c) {
    Covers_Stats s = {0};
    if (!c) return s;
    s.resident = c->resident;
    s.capacity = COV_SLOTS;
    platform_mutex_lock(&c->mutex);
    s.pending = c->req_count + c->in_flight + c->done_count +
                (c->art_req_pending ? 1 : 0) + (c->art_done_valid ? 1 : 0);
    s.prefetch_left = c->pf_count - c->pf_next + (c->pf_active ? 1 : 0);
    platform_mutex_unlock(&c->mutex);
    return s;
}
