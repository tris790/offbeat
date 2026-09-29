#ifndef CORE_FONT_H
#define CORE_FONT_H

#include "math.h"
#include "memory.h"
#include "string.h"
#include "renderer.h"

/*
 * Text rendering with a dynamic glyph cache.
 *
 * Glyphs are rasterized on demand (stb_truetype, coverage bitmaps) at the
 * exact pixel size they are drawn at, with 3 horizontal subpixel phases, and
 * packed into one R8 atlas. That keeps small UI text crisp (no SDF blur) and
 * memory flat: only glyphs actually on screen are ever rasterized.
 *
 * A Core_Font is a fallback chain of font files: the first that contains a
 * codepoint wins (e.g. Outfit -> Noto Sans -> Noto Sans CJK). Font files are
 * memory-mapped through a caller-supplied loader, so a 20 MB CJK font costs
 * only the pages actually touched, and nothing until a CJK glyph appears.
 */

typedef struct Core_Text Core_Text;
typedef struct { u32 id; } Core_Font;

/* Map a whole file read-only. Returns 0 on failure. */
typedef const u8 *(*Core_FileMapProc)(const char *path, u64 *out_size);

Core_Text *core_text_create(Core_Arena *arena, Core_Renderer *r, Core_FileMapProc map_file);

/* Register a fallback chain (paths tried in order; missing files skipped).
   Returns a font with id 0 if no file could be loaded at all. */
Core_Font  core_text_font(Core_Text *t, const char *const *paths, u32 count);
b32        core_text_font_ok(Core_Text *t, Core_Font f);

/* Call once per frame before drawing (recycles the atlas if it filled up). */
void core_text_begin_frame(Core_Text *t);

typedef struct {
    f32 ascent;      /* baseline to top, px (positive)       */
    f32 descent;     /* baseline to bottom, px (negative)    */
    f32 line_height; /* ascent - descent + line gap          */
    f32 cap_height;  /* height of 'H'                        */
    f32 x_height;    /* height of 'x'                        */
} Core_FontMetrics;

Core_FontMetrics core_text_metrics(Core_Text *t, Core_Font f, f32 px);

/* Draw UTF-8 `s` with its baseline-left at `pos`. Returns the advance width. */
f32 core_text_draw(Core_Text *t, Core_Font f, Core_String s, vec2 pos, f32 px, vec4 color);
/* Same, but truncates with an ellipsis to fit `max_w`. */
f32 core_text_draw_fit(Core_Text *t, Core_Font f, Core_String s, vec2 pos, f32 px,
                       vec4 color, f32 max_w);
/* Width of `s` in px. */
f32 core_text_measure(Core_Text *t, Core_Font f, Core_String s, f32 px);
/* Byte offset in `s` of the caret position closest to x (relative to start). */
u64 core_text_hit(Core_Text *t, Core_Font f, Core_String s, f32 px, f32 x);

/* Stats for the debug overlay. */
typedef struct { u32 glyphs; u32 atlas_resets; f32 atlas_fill; } Core_TextStats;
Core_TextStats core_text_stats(Core_Text *t);

#endif /* CORE_FONT_H */
