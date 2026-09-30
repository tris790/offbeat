/*
 * Remote thumbnails: see ytthumbs.h.
 */

#include "ytthumbs.h"

#include "../core/memory.h"
#include "../platform/platform.h"
#include "../third_party/stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define YT_SLOT   96                 /* px, square                             */
#define YT_COLS   12
#define YT_ROWS   11
#define YT_SLOTS  (YT_COLS * YT_ROWS)
#define YT_QUEUE  192
#define YT_DONE   12

typedef enum { SLOT_FREE = 0, SLOT_PENDING, SLOT_READY, SLOT_MISSING } Slot_State;

typedef struct {
    char vid[12];
    u8   state;
    u32  used;        /* frame of last use, for LRU */
    f32  age;
} Slot;

typedef struct { char vid[12]; u32 slot; } Request;
typedef struct { u32 slot; char vid[12]; u8 *rgba; } Finished; /* rgba == 0: failed */

struct Ythumbs {
    Core_Renderer *r;
    Core_Texture   atlas;
    char           dir[1024];
    Slot           slots[YT_SLOTS];
    u32            frame;

    Platform_Mutex mu;
    Platform_Cond  cv;
    b32            quit;
    Platform_Thread *thread;
    Request        queue[YT_QUEUE];
    u32            queue_len;
    Finished       done[YT_DONE];
    u32            done_len;
};

/* Center-crop to a square and box-downscale to YT_SLOT. */
static void to_slot(const u8 *src, u32 sw, u32 sh, u32 ch, u8 *dst) {
    u32 side = sw < sh ? sw : sh;
    u32 ox = (sw - side) / 2, oy = (sh - side) / 2;
    for (u32 y = 0; y < YT_SLOT; y++) {
        u32 y0 = oy + (u32)((u64)y * side / YT_SLOT), y1 = oy + (u32)((u64)(y + 1) * side / YT_SLOT);
        if (y1 <= y0) y1 = y0 + 1;
        for (u32 x = 0; x < YT_SLOT; x++) {
            u32 x0 = ox + (u32)((u64)x * side / YT_SLOT), x1 = ox + (u32)((u64)(x + 1) * side / YT_SLOT);
            if (x1 <= x0) x1 = x0 + 1;
            u32 acc[3] = {0, 0, 0}, n = 0;
            for (u32 yy = y0; yy < y1 && yy < sh; yy++)
                for (u32 xx = x0; xx < x1 && xx < sw; xx++) {
                    const u8 *p = src + ((u64)yy * sw + xx) * ch;
                    acc[0] += p[0]; acc[1] += p[ch > 2 ? 1 : 0]; acc[2] += p[ch > 2 ? 2 : 0];
                    n++;
                }
            if (!n) n = 1;
            u8 *d = dst + ((u64)y * YT_SLOT + x) * 4;
            d[0] = (u8)(acc[0] / n); d[1] = (u8)(acc[1] / n); d[2] = (u8)(acc[2] / n); d[3] = 255;
        }
    }
}

/* Make sure the JPEG is on disk (downloading it with curl if needed) and decode it. */
static u8 *fetch_and_decode(Ythumbs *t, const char *vid) {
    char path[1200], part[1210], url[96];
    snprintf(path, sizeof(path), "%s/%s.jpg", t->dir, vid);
    Platform_FileInfo fi = platform_file_info(path);
    if (!fi.exists || fi.size == 0) {
        snprintf(part, sizeof(part), "%s.part", path);
        snprintf(url, sizeof(url), "https://i.ytimg.com/vi/%s/mqdefault.jpg", vid);
        const char *argv[] = { "curl", "-sfL", "--max-time", "15", "-o", part, url, 0 };
        Platform_Process *p = platform_process_spawn(argv);
        if (!p) return 0;
        char line[128];
        while (platform_process_read_line(p, line, sizeof(line), 0.5) != -1) {
            if (t->quit) platform_process_kill(p);
        }
        s32 code = platform_process_finish(p);
        if (code != 0 || !platform_file_info(part).exists) { platform_file_remove(part); return 0; }
        platform_file_rename(part, path);
    }
    Core_Arena scratch;
    if (!core_arena_init(&scratch, CORE_MB(8))) return 0;
    Core_String file = platform_file_read_all(&scratch, path);
    u8 *out = 0;
    if (file.len) {
        int w, h, ch;
        u8 *px = stbi_load_from_memory(file.str, (int)file.len, &w, &h, &ch, 3);
        if (px && w > 0 && h > 0) {
            out = core_heap_alloc((u64)YT_SLOT * YT_SLOT * 4);
            to_slot(px, (u32)w, (u32)h, 3, out);
        }
        if (px) stbi_image_free(px);
    }
    core_arena_release(&scratch);
    return out;
}

static void worker_main(void *arg) {
    Ythumbs *t = arg;
    platform_thread_set_background();
    platform_mutex_lock(&t->mu);
    while (!t->quit) {
        if (!t->queue_len || t->done_len == YT_DONE) { platform_cond_wait(&t->cv, &t->mu); continue; }
        Request req = t->queue[--t->queue_len]; /* newest first: what is on screen now */
        platform_mutex_unlock(&t->mu);
        u8 *px = fetch_and_decode(t, req.vid);
        platform_mutex_lock(&t->mu);
        if (t->done_len < YT_DONE) {
            Finished *f = &t->done[t->done_len++];
            f->slot = req.slot;
            memcpy(f->vid, req.vid, sizeof(f->vid));
            f->rgba = px;
        } else {
            core_heap_free(px);
        }
    }
    platform_mutex_unlock(&t->mu);
}

Ythumbs *ythumbs_create(Core_Renderer *r, const char *cache_dir) {
    Ythumbs *t = core_heap_calloc(sizeof(*t));
    t->r = r;
    snprintf(t->dir, sizeof(t->dir), "%s/ytthumbs", cache_dir);
    platform_make_dirs(t->dir);
    platform_mutex_init(&t->mu);
    platform_cond_init(&t->cv);
    t->atlas = core_texture_create(r, CORE_TEXTURE_RGBA8, YT_COLS * YT_SLOT, YT_ROWS * YT_SLOT, 0, false);
    t->thread = platform_thread_start(worker_main, t, "yt-thumbs");
    return t;
}

void ythumbs_destroy(Ythumbs *t) {
    if (!t) return;
    platform_mutex_lock(&t->mu);
    t->quit = true;
    platform_mutex_unlock(&t->mu);
    platform_cond_broadcast(&t->cv);
    platform_thread_join(t->thread);
    for (u32 i = 0; i < t->done_len; i++) core_heap_free(t->done[i].rgba);
    core_texture_destroy(t->r, t->atlas);
    core_heap_free(t);
}

void ythumbs_update(Ythumbs *t) {
    t->frame++;
    platform_mutex_lock(&t->mu);
    u32 n = t->done_len < 3 ? t->done_len : 3; /* a few per frame keeps uploads smooth */
    Finished batch[3];
    memcpy(batch, t->done, sizeof(Finished) * n);
    memmove(t->done, t->done + n, sizeof(Finished) * (t->done_len - n));
    t->done_len -= n;
    platform_mutex_unlock(&t->mu);
    if (n) platform_cond_broadcast(&t->cv);
    for (u32 i = 0; i < n; i++) {
        Slot *s = &t->slots[batch[i].slot];
        if (strcmp(s->vid, batch[i].vid) != 0) { core_heap_free(batch[i].rgba); continue; } /* evicted meanwhile */
        if (batch[i].rgba) {
            core_texture_update(t->r, t->atlas, (batch[i].slot % YT_COLS) * YT_SLOT, (batch[i].slot / YT_COLS) * YT_SLOT,
                                YT_SLOT, YT_SLOT, batch[i].rgba);
            s->state = SLOT_READY;
            s->age = 0;
        } else {
            s->state = SLOT_MISSING;
        }
        core_heap_free(batch[i].rgba);
    }
}

Ythumb ythumbs_get(Ythumbs *t, const char *vid, f32 dt) {
    Ythumb out = { .tex = t->atlas };
    Slot *found = 0, *victim = 0;
    for (u32 i = 0; i < YT_SLOTS; i++) {
        Slot *s = &t->slots[i];
        if (s->state != SLOT_FREE && strcmp(s->vid, vid) == 0) { found = s; break; }
        if (s->state == SLOT_PENDING) continue;
        if (!victim || s->state == SLOT_FREE || (victim->state != SLOT_FREE && s->used < victim->used)) victim = s;
    }
    if (!found) {
        if (!victim || victim->used == t->frame) return out; /* every slot is in use this frame */
        u32 idx = (u32)(victim - t->slots);
        memset(victim, 0, sizeof(*victim));
        snprintf(victim->vid, sizeof(victim->vid), "%s", vid);
        victim->state = SLOT_PENDING;
        victim->used = t->frame;
        platform_mutex_lock(&t->mu);
        if (t->queue_len == YT_QUEUE) memmove(t->queue, t->queue + 1, sizeof(Request) * (YT_QUEUE - 1)), t->queue_len--;
        Request *q = &t->queue[t->queue_len++];
        snprintf(q->vid, sizeof(q->vid), "%s", vid);
        q->slot = idx;
        platform_mutex_unlock(&t->mu);
        platform_cond_broadcast(&t->cv);
        return out;
    }
    found->used = t->frame;
    if (found->state == SLOT_MISSING) { out.missing = true; return out; }
    if (found->state != SLOT_READY) return out;
    found->age += dt;
    u32 idx = (u32)(found - t->slots);
    f32 aw = (f32)(YT_COLS * YT_SLOT), ah = (f32)(YT_ROWS * YT_SLOT);
    f32 px = (f32)((idx % YT_COLS) * YT_SLOT), py = (f32)((idx / YT_COLS) * YT_SLOT);
    out.uv0 = vec2_make(px / aw, py / ah);
    out.uv1 = vec2_make((px + YT_SLOT) / aw, (py + YT_SLOT) / ah);
    out.ready = true;
    out.age = found->age;
    return out;
}
