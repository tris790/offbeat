#ifndef GAME_COVERS_H
#define GAME_COVERS_H

#include "../core/math.h"
#include "../core/renderer.h"
#include "library.h"

/*
 * Album art.
 *
 * Thumbnails (list rows, search results) live in one GPU atlas of fixed-size
 * slots with LRU eviction, so GPU/CPU memory is bounded no matter how large
 * the library is. Worker threads produce them: first from the on-disk
 * thumbnail cache (~/.cache/offbeat/thumbs, tiny JPEGs), otherwise by
 * decoding the embedded picture, downscaling and writing the cache. The UI
 * never waits: a request returns "not ready" and the UI draws a placeholder
 * that fades into the art when it arrives (usually next frame).
 *
 * Requests only copy what they need (path, offset, size, hash) so Library
 * snapshots can be swapped/freed while work is in flight.
 *
 * The now-playing artwork is decoded at higher resolution with mipmaps (the
 * high mips double as a free blur for backgrounds) plus a small color
 * palette extracted from it for theming.
 */

#define COVER_THUMB_SIZE 64   /* px, square */
#define COVER_ART_SIZE   512  /* px, square */

typedef struct Covers Covers;

Covers *covers_create(Core_Renderer *r, const char *cache_dir, u32 worker_count);
void    covers_destroy(Covers *c);

/* UI thread, once per frame: upload finished work (bounded per frame), age
   entries, drop requests nobody asked for recently. */
void    covers_update(Covers *c, f32 dt);

typedef struct {
    Core_Texture tex;   /* atlas texture                                     */
    vec2 uv0, uv1;
    b32  ready;
    b32  missing;       /* track has no art (draw a generated placeholder)   */
    f32  age;           /* seconds since it became ready (fade-in)           */
} Cover_Thumb;

/* Returns the thumbnail if resident, else queues it (latest requests are
   served first) and returns ready=false. Cheap; call every frame for every
   visible row. */
Cover_Thumb covers_thumb(Covers *c, const Lib_Track *t);

typedef struct {
    Core_Texture tex;     /* COVER_ART_SIZE^2, mipmapped                      */
    b32  ready;
    b32  missing;
    f32  age;             /* seconds since ready (crossfade)                  */
    u64  track_hash;
    vec3 palette[4];      /* 0 = darkest dominant ... 3 = most vibrant, 0..1 */
} Cover_Art;

/* Make `t` the now-playing artwork (no-op if already). The previous art stays
   available for crossfading until the new one is ready. */
void      covers_set_art(Covers *c, const Lib_Track *t);
Cover_Art covers_art(Covers *c);       /* current (may not be ready yet)      */
Cover_Art covers_art_prev(Covers *c);  /* previous, for crossfade             */

/* Generate missing disk thumbnails for the whole library at low priority so
   later scrolling is instant. Copies what it needs; safe to call again with a
   newer snapshot (restarts). */
void covers_prefetch_library(Covers *c, const Library *lib);

/* Stats for the debug overlay. */
typedef struct { u32 resident; u32 capacity; u32 pending; u32 prefetch_left; } Covers_Stats;
Covers_Stats covers_stats(Covers *c);

#endif /* GAME_COVERS_H */
