#ifndef GAME_YTTHUMBS_H
#define GAME_YTTHUMBS_H

#include "../core/math.h"
#include "../core/renderer.h"

/*
 * Thumbnails for the download page's results.
 *
 * YouTube serves one small JPEG per video id. A worker thread fetches it with
 * curl (skipped quietly when curl is missing: rows keep their placeholder),
 * caches the file on disk, center-crops it to a square and downscales it; the
 * UI thread uploads the finished pixels into one GPU atlas of fixed slots
 * with LRU eviction, so memory stays bounded however many results scroll by.
 * Requests never block: ytthumbs_get() returns "not ready" and queues the
 * work, like covers_thumb() does for library art.
 */

typedef struct Ythumbs Ythumbs;

Ythumbs *ythumbs_create(Core_Renderer *r, const char *cache_dir);
void     ythumbs_destroy(Ythumbs *t);

/* UI thread, once per frame: upload what the worker finished. */
void     ythumbs_update(Ythumbs *t);

typedef struct {
    Core_Texture tex;
    vec2 uv0, uv1;
    b32  ready;
    b32  missing;    /* could not be fetched: keep the placeholder          */
    f32  age;        /* seconds since ready (fade-in)                        */
} Ythumb;

Ythumb   ythumbs_get(Ythumbs *t, const char *video_id, f32 dt);

#endif /* GAME_YTTHUMBS_H */
