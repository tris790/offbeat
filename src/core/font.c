#include "font.h"

#include "third_party/stb_truetype.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ATLAS_W        1024
#define ATLAS_H        1024
#define MAX_FACES      16
#define MAX_FONTS      8
#define MAX_CHAIN      4
#define GLYPH_CAP      8192   /* hash slots, power of two */
#define KERN_CAP       4096   /* kerning pair cache slots */
#define MAX_SHELVES    64
#define SUBPIXELS      3
#define SCRATCH_DIM    192    /* largest glyph bitmap we rasterize */

typedef struct {
    char path[256];
    b32  tried;
    b32  ok;
    stbtt_fontinfo info;
} Face;

typedef struct {
    u64 key;          /* 0 = empty slot                        */
    u16 x, y, w, h;   /* atlas rect                            */
    s16 xoff, yoff;   /* bitmap offset from pen/baseline       */
    f32 advance;      /* px                                    */
    u16 glyph;        /* glyph index in `face`                 */
    u8  face;
    u8  drawable;     /* has pixels in the atlas               */
} Glyph;

typedef struct { u64 key; f32 value; } Kern;

typedef struct { u16 y, h, x; } Shelf;

struct Core_Text {
    Core_Renderer   *r;
    Core_FileMapProc map_file;
    Core_Texture     atlas;

    Face faces[MAX_FACES];
    u32  face_count;

    u8   chains[MAX_FONTS][MAX_CHAIN];
    u8   chain_len[MAX_FONTS];
    u32  font_count;

    Glyph *glyphs;
    u32    glyph_count;
    Kern  *kerns;

    Shelf shelves[MAX_SHELVES];
    u32   shelf_count;
    u32   shelf_bottom;
    b32   reset_pending;
    u32   resets;

    u8 scratch[SCRATCH_DIM * SCRATCH_DIM];
};

static b32 face_load(Core_Text *t, u32 i) {
    Face *f = &t->faces[i];
    if (f->tried) return f->ok;
    f->tried = true;
    u64 size = 0;
    const u8 *data = t->map_file(f->path, &size);
    if (!data || size < 12) return false;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, data, offset)) {
        fprintf(stderr, "[font] cannot parse '%s'\n", f->path);
        return false;
    }
    f->ok = true;
    return true;
}

Core_Text *core_text_create(Core_Arena *arena, Core_Renderer *r, Core_FileMapProc map_file) {
    Core_Text *t = core_push_struct(arena, Core_Text);
    t->r = r;
    t->map_file = map_file;
    t->glyphs = core_push_array(arena, Glyph, GLYPH_CAP);
    t->kerns  = core_push_array(arena, Kern, KERN_CAP);
    t->atlas  = core_texture_create(r, CORE_TEXTURE_R8, ATLAS_W, ATLAS_H, 0, false);
    t->font_count = 1; /* id 0 = invalid */
    return t;
}

Core_Font core_text_font(Core_Text *t, const char *const *paths, u32 count) {
    if (t->font_count >= MAX_FONTS) return (Core_Font){0};
    u32 id = t->font_count;
    u32 n = 0;
    for (u32 i = 0; i < count && n < MAX_CHAIN; i++) {
        /* share faces between chains */
        u32 fi = 0;
        for (; fi < t->face_count; fi++)
            if (strcmp(t->faces[fi].path, paths[i]) == 0) break;
        if (fi == t->face_count) {
            if (t->face_count == MAX_FACES) break;
            snprintf(t->faces[fi].path, sizeof(t->faces[fi].path), "%s", paths[i]);
            t->face_count++;
        }
        t->chains[id][n++] = (u8)fi;
    }
    t->chain_len[id] = (u8)n;
    /* The primary face loads eagerly (metrics); fallbacks load on first use. */
    b32 any = false;
    for (u32 i = 0; i < n && !any; i++) any = face_load(t, t->chains[id][i]);
    if (!any) return (Core_Font){0};
    t->font_count++;
    return (Core_Font){ id };
}

b32 core_text_font_ok(Core_Text *t, Core_Font f) {
    return f.id > 0 && f.id < t->font_count;
}

/* First loaded face of the chain: supplies metrics. */
static Face *primary_face(Core_Text *t, Core_Font f) {
    for (u32 i = 0; i < t->chain_len[f.id]; i++) {
        u32 fi = t->chains[f.id][i];
        if (face_load(t, fi)) return &t->faces[fi];
    }
    return 0;
}

static void atlas_reset(Core_Text *t) {
    memset(t->glyphs, 0, sizeof(Glyph) * GLYPH_CAP);
    t->glyph_count = 0;
    t->shelf_count = 0;
    t->shelf_bottom = 1;
    t->resets++;
}

void core_text_begin_frame(Core_Text *t) {
    if (t->reset_pending) {
        atlas_reset(t);
        t->reset_pending = false;
    }
}

/* Shelf packer. Returns false when the atlas is full. */
static b32 atlas_alloc(Core_Text *t, u32 w, u32 h, u16 *ox, u16 *oy) {
    if (t->shelf_bottom == 0) t->shelf_bottom = 1;
    Shelf *best = 0;
    for (u32 i = 0; i < t->shelf_count; i++) {
        Shelf *s = &t->shelves[i];
        if (s->h >= h && s->h <= h + h / 3 + 2 && s->x + w + 1 <= ATLAS_W) {
            if (!best || s->h < best->h) best = s;
        }
    }
    if (!best) {
        if (t->shelf_count == MAX_SHELVES || t->shelf_bottom + h + 1 > ATLAS_H) return false;
        best = &t->shelves[t->shelf_count++];
        best->y = (u16)t->shelf_bottom;
        best->h = (u16)(h + 1);
        best->x = 1;
        t->shelf_bottom += h + 2;
    }
    *ox = best->x;
    *oy = best->y;
    best->x = (u16)(best->x + w + 1);
    return true;
}

CORE_INLINE u64 hash_u64(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static f32 face_scale(Face *f, f32 px) {
    return stbtt_ScaleForMappingEmToPixels(&f->info, px);
}

/* Look up (rasterizing on miss) the glyph for `cp` at integer size `ipx` and
   subpixel phase `sub`. Returns 0 only if nothing can render it. */
static Glyph *glyph_get(Core_Text *t, Core_Font f, u32 cp, u32 ipx, u32 sub) {
    u64 key = ((u64)cp) | ((u64)sub << 21) | ((u64)ipx << 24) | ((u64)f.id << 40) | (1ULL << 63);
    u32 mask = GLYPH_CAP - 1;
    u32 i = (u32)hash_u64(key) & mask;
    for (;;) {
        Glyph *g = &t->glyphs[i];
        if (g->key == key) return g;
        if (g->key == 0) break;
        i = (i + 1) & mask;
    }
    if (t->glyph_count >= GLYPH_CAP * 3 / 4) {
        t->reset_pending = true;
        return 0;
    }

    /* Find the first face in the chain that has this codepoint. */
    u32 face_i = 0, glyph_index = 0;
    b32 found = false;
    for (u32 c = 0; c < t->chain_len[f.id] && !found; c++) {
        u32 fi = t->chains[f.id][c];
        if (!face_load(t, fi)) continue;
        int gi = stbtt_FindGlyphIndex(&t->faces[fi].info, (int)cp);
        if (gi > 0 || c + 1 == t->chain_len[f.id]) {
            if (gi > 0) { face_i = fi; glyph_index = (u32)gi; found = true; }
        }
    }
    if (!found) {
        /* tofu: fall back to the primary face's .notdef (index 0) */
        Face *pf = primary_face(t, f);
        if (!pf) return 0;
        face_i = (u32)(pf - t->faces);
        glyph_index = 0;
    }
    Face *face = &t->faces[face_i];
    f32 scale = face_scale(face, (f32)ipx);
    f32 shift = (f32)sub / (f32)SUBPIXELS;

    int adv, lsb;
    stbtt_GetGlyphHMetrics(&face->info, (int)glyph_index, &adv, &lsb);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&face->info, (int)glyph_index, scale, scale, shift, 0,
                                    &x0, &y0, &x1, &y1);
    u32 w = (u32)(x1 - x0), h = (u32)(y1 - y0);

    Glyph g = {0};
    g.key = key;
    g.advance = (f32)adv * scale;
    g.xoff = (s16)x0;
    g.yoff = (s16)y0;
    g.face = (u8)face_i;
    g.glyph = (u16)glyph_index;

    if (w > 0 && h > 0 && w <= SCRATCH_DIM && h <= SCRATCH_DIM) {
        u16 ax, ay;
        if (!atlas_alloc(t, w, h, &ax, &ay)) {
            t->reset_pending = true;
            return 0;
        }
        stbtt_MakeGlyphBitmapSubpixel(&face->info, t->scratch, (int)w, (int)h, (int)w,
                                      scale, scale, shift, 0, (int)glyph_index);
        core_texture_update(t->r, t->atlas, ax, ay, w, h, t->scratch);
        g.x = ax; g.y = ay; g.w = (u16)w; g.h = (u16)h;
        g.drawable = 1;
    }

    t->glyphs[i] = g;
    t->glyph_count++;
    return &t->glyphs[i];
}

static f32 kern_get(Core_Text *t, const Glyph *a, const Glyph *b, u32 ipx) {
    if (a->face != b->face) return 0;
    u64 key = ((u64)a->glyph) | ((u64)b->glyph << 16) | ((u64)a->face << 32) | ((u64)ipx << 40) | (1ULL << 63);
    u32 mask = KERN_CAP - 1;
    u32 i = (u32)hash_u64(key) & mask;
    for (u32 probe = 0; probe < 8; probe++) {
        Kern *k = &t->kerns[i];
        if (k->key == key) return k->value;
        if (k->key == 0) {
            Face *face = &t->faces[a->face];
            f32 v = (f32)stbtt_GetGlyphKernAdvance(&face->info, a->glyph, b->glyph) * face_scale(face, (f32)ipx);
            k->key = key;
            k->value = v;
            return v;
        }
        i = (i + 1) & mask;
    }
    /* cache neighborhood full: compute without caching */
    Face *face = &t->faces[a->face];
    return (f32)stbtt_GetGlyphKernAdvance(&face->info, a->glyph, b->glyph) * face_scale(face, (f32)ipx);
}

Core_FontMetrics core_text_metrics(Core_Text *t, Core_Font f, f32 px) {
    Core_FontMetrics m = {0};
    if (!core_text_font_ok(t, f)) return m;
    Face *face = primary_face(t, f);
    if (!face) return m;
    f32 s = face_scale(face, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&face->info, &asc, &desc, &gap);
    m.ascent = (f32)asc * s;
    m.descent = (f32)desc * s;
    m.line_height = (f32)(asc - desc + gap) * s;
    int x0, y0, x1, y1;
    if (stbtt_GetCodepointBox(&face->info, 'H', &x0, &y0, &x1, &y1)) m.cap_height = (f32)y1 * s;
    else m.cap_height = m.ascent * 0.7f;
    if (stbtt_GetCodepointBox(&face->info, 'x', &x0, &y0, &x1, &y1)) m.x_height = (f32)y1 * s;
    else m.x_height = m.cap_height * 0.7f;
    return m;
}

/* Core layout loop shared by draw and measure. When `draw` is false nothing
   is emitted. Stops (returning the byte offset) once the pen would pass
   `stop_x`; `*out_w` receives the width consumed. */
static u64 layout(Core_Text *t, Core_Font f, Core_String s, vec2 pos, f32 px, vec4 color,
                  b32 draw, f32 stop_x, f32 *out_w) {
    u32 ipx = (u32)(px + 0.5f);
    if (ipx == 0 || !core_text_font_ok(t, f)) { *out_w = 0; return 0; }
    f32 inv_w = 1.0f / ATLAS_W, inv_h = 1.0f / ATLAS_H;
    f32 pen = 0;
    f32 base_y = floorf(pos.y + 0.5f);
    Glyph prev = {0};
    b32 has_prev = false;
    u64 at = 0;
    while (at < s.len) {
        Core_Utf8Decode d = core_utf8_decode(s, at);
        u32 cp = d.codepoint;
        if (cp == '\n' || cp == '\r') cp = ' ';

        f32 x = pos.x + pen;
        u32 sub = (u32)((x - floorf(x)) * SUBPIXELS) % SUBPIXELS;
        Glyph *g = glyph_get(t, f, cp, ipx, sub);
        if (!g) { at += d.size; continue; }
        if (has_prev) pen += kern_get(t, &prev, g, ipx);

        f32 adv = g->advance;
        if (pen + adv * 0.5f > stop_x) break;

        if (draw && g->drawable) {
            x = pos.x + pen;
            f32 gx = floorf(x) + g->xoff;
            f32 gy = base_y + g->yoff;
            core_draw_glyph(t->r, t->atlas, vec2_make(gx, gy), vec2_make(g->w, g->h),
                            vec2_make(g->x * inv_w, g->y * inv_h),
                            vec2_make((g->x + g->w) * inv_w, (g->y + g->h) * inv_h), color);
        }
        pen += adv;
        prev = *g;
        has_prev = true;
        at += d.size;
    }
    *out_w = pen;
    return at;
}

f32 core_text_draw(Core_Text *t, Core_Font f, Core_String s, vec2 pos, f32 px, vec4 color) {
    f32 w;
    layout(t, f, s, pos, px, color, true, 1e30f, &w);
    return w;
}

f32 core_text_measure(Core_Text *t, Core_Font f, Core_String s, f32 px) {
    f32 w;
    layout(t, f, s, vec2_zero(), px, (vec4){0}, false, 1e30f, &w);
    return w;
}

u64 core_text_hit(Core_Text *t, Core_Font f, Core_String s, f32 px, f32 x) {
    f32 w;
    return layout(t, f, s, vec2_zero(), px, (vec4){0}, false, x, &w);
}

f32 core_text_draw_fit(Core_Text *t, Core_Font f, Core_String s, vec2 pos, f32 px,
                       vec4 color, f32 max_w) {
    f32 full = core_text_measure(t, f, s, px);
    if (full <= max_w) return core_text_draw(t, f, s, pos, px, color);

    Core_String ell = core_str_lit("\xE2\x80\xA6"); /* … */
    f32 ell_w = core_text_measure(t, f, ell, px);
    f32 room = max_w - ell_w;
    if (room <= 0) return 0;
    /* Largest prefix that fits (layout stops at half-glyph; walk back to be
       safe), then trim trailing spaces so the ellipsis hugs the text. */
    f32 w;
    u64 cut = layout(t, f, s, vec2_zero(), px, (vec4){0}, false, room, &w);
    Core_String prefix = core_str_prefix(s, cut);
    while (prefix.len && core_text_measure(t, f, prefix, px) > room) {
        u64 n = prefix.len - 1;
        while (n > 0 && (prefix.str[n] & 0xC0) == 0x80) n--;
        prefix.len = n;
    }
    while (prefix.len && (prefix.str[prefix.len - 1] == ' ' || prefix.str[prefix.len - 1] == '-'))
        prefix.len--;
    f32 pw = core_text_draw(t, f, prefix, pos, px, color);
    core_text_draw(t, f, ell, vec2_make(pos.x + pw, pos.y), px, color);
    return pw + ell_w;
}

Core_TextStats core_text_stats(Core_Text *t) {
    return (Core_TextStats){
        .glyphs = t->glyph_count,
        .atlas_resets = t->resets,
        .atlas_fill = (f32)t->shelf_bottom / (f32)ATLAS_H,
    };
}
