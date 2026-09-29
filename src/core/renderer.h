#ifndef CORE_RENDERER_H
#define CORE_RENDERER_H

#include "math.h"
#include "memory.h"

/*
 * Renderer abstraction.
 *
 * The interface here is backend-agnostic: it knows nothing about OpenGL. A
 * backend (renderer_gl.c) implements the functions declared here.
 *
 * Drawing model: immediate-style calls accumulate *instances* (one per quad)
 * between begin_frame / end_frame. Every shape -- rects, rounded boxes with
 * borders/shadows/gradients, circles, arcs, capsules, triangles, images,
 * glyphs -- is resolved analytically in one über-shader, so a whole UI frame is
 * usually a handful of instanced draw calls. Up to 4 textures are bound per
 * batch; the batch flushes when a 5th is needed or a custom effect is drawn.
 *
 * Colors are straight (non-premultiplied) RGBA in 0..1; output is
 * premultiplied, so a window with an alpha channel composites correctly.
 * All coordinates are pixels, origin top-left.
 */

typedef struct Core_Renderer Core_Renderer;

/* GPU texture handle. Opaque; created by the backend. */
typedef struct {
    u32 id;       /* backend-specific (GL texture name) */
    u32 width;
    u32 height;
    u32 format;   /* CORE_TEXTURE_* */
    u32 mip_levels;
} Core_Texture;

enum {
    CORE_TEXTURE_RGBA8 = 0,
    CORE_TEXTURE_R8    = 1,
};

/* ---- lifecycle ---- */

typedef void *(*Core_GlLoadProc)(const char *name);
Core_Renderer *core_renderer_create(Core_Arena *arena, Core_GlLoadProc load_proc);
void           core_renderer_destroy(Core_Renderer *r);

/* Update the drawable size / set up the orthographic projection (pixels). */
void core_renderer_resize(Core_Renderer *r, u32 width, u32 height);

/* Frame boundaries. clear_color is straight RGBA in 0..1. */
void core_renderer_begin_frame(Core_Renderer *r, vec4 clear_color);
void core_renderer_end_frame(Core_Renderer *r);

/* Render into an offscreen color target of the given size instead of the
   window (headless screenshots). Call before begin_frame. */
b32  core_renderer_use_offscreen(Core_Renderer *r, u32 width, u32 height);
/* Read back the last rendered frame as tightly packed RGBA8, top row first. */
void core_renderer_read_pixels(Core_Renderer *r, u32 width, u32 height, void *out_rgba);

/* Multiply everything drawn so far by a rounded-box coverage mask (clears the
   corners of a transparent window). Call after end_frame. */
void core_renderer_mask_rounded(Core_Renderer *r, vec2 pos, vec2 size, f32 radius);

/* Per-frame counters for the debug overlay. */
typedef struct { u32 draw_calls; u32 instances; } Core_RenderStats;
Core_RenderStats core_renderer_stats(Core_Renderer *r);

/* ---- textures ---- */

/* Create a texture from tightly packed pixels (pixels may be 0). When
   `mipmapped` is set a full mip chain is allocated; call
   core_texture_generate_mips after uploading. */
Core_Texture core_texture_create(Core_Renderer *r, u32 format, u32 w, u32 h,
                                 const void *pixels, b32 mipmapped);
Core_Texture core_texture_create_rgba(Core_Renderer *r, u32 w, u32 h, const void *pixels);
Core_Texture core_texture_create_r8(Core_Renderer *r, u32 w, u32 h, const void *pixels);
/* Upload a sub-rectangle (tightly packed, same format as the texture). */
void         core_texture_update(Core_Renderer *r, Core_Texture tex,
                                 u32 x, u32 y, u32 w, u32 h, const void *pixels);
void         core_texture_generate_mips(Core_Renderer *r, Core_Texture tex);
void         core_texture_destroy(Core_Renderer *r, Core_Texture tex);

/* ---- clipping ----
   A clip rectangle applies to everything drawn until popped. Nested clips
   intersect. Cheap: it's a per-instance field, not a GL state change. */
void core_clip_push(Core_Renderer *r, vec2 pos, vec2 size);
void core_clip_pop(Core_Renderer *r);

/* ---- 2D drawing ---- */

/* Blend: normal alpha-over, or additive light (glows, sparks). */
typedef enum { CORE_BLEND_NORMAL = 0, CORE_BLEND_ADD = 1 } Core_Blend;
void core_set_blend(Core_Renderer *r, Core_Blend blend);

/* Recolor: a row-major 3x3 matrix applied to the RGB of every shape and
   glyph drawn afterwards (images keep their colors). Theming uses it to
   rotate the palette; 0 restores identity. Cheap: applied on the CPU when
   the instance is recorded. */
void core_set_color_matrix(Core_Renderer *r, const f32 *m);

/* Solid / gradient rectangles. */
void core_draw_rect(Core_Renderer *r, vec2 pos, vec2 size, vec4 color);
/* Vertical gradient top->bottom (or horizontal left->right). */
void core_draw_rect_gradient(Core_Renderer *r, vec2 pos, vec2 size, vec4 c0, vec4 c1, b32 horizontal);

/* Rounded box. `radius` in px. */
void core_draw_rect_rounded(Core_Renderer *r, vec2 pos, vec2 size, f32 radius, vec4 color);

/* Full rounded box description: fill (optionally gradient), border, shadow. */
typedef struct {
    f32  radius;
    vec4 fill;          /* fill color (a=0 for none)                          */
    vec4 fill2;         /* gradient end color; used when gradient != 0        */
    u32  gradient;      /* 0 none, 1 vertical, 2 horizontal                   */
    f32  border;        /* border width in px (0 = none)                      */
    vec4 border_color;
} Core_BoxStyle;
void core_draw_box(Core_Renderer *r, vec2 pos, vec2 size, const Core_BoxStyle *style);

/* Soft drop shadow / glow for a rounded box: `blur` is the falloff width.  */
void core_draw_shadow(Core_Renderer *r, vec2 pos, vec2 size, f32 radius, f32 blur, vec4 color);

/* Filled circle / ring (stroke > 0) with optional soft edge `blur`. */
void core_draw_circle(Core_Renderer *r, vec2 center, f32 radius, vec4 color);
void core_draw_circle_ex(Core_Renderer *r, vec2 center, f32 radius, f32 stroke, f32 blur, vec4 color);
/* Arc stroke with round caps from angle `a0` sweeping `sweep` radians
   (clockwise on screen, 0 = +x axis / 3 o'clock). */
void core_draw_arc(Core_Renderer *r, vec2 center, f32 radius, f32 stroke,
                   f32 a0, f32 sweep, vec4 color);

/* Capsule line segment of `thickness` px with round caps; `c1` is the color
   at `b` (gradient along the line). */
void core_draw_line(Core_Renderer *r, vec2 a, vec2 b, f32 thickness, vec4 color);
void core_draw_line_ex(Core_Renderer *r, vec2 a, vec2 b, f32 thickness, f32 blur, vec4 c0, vec4 c1);

/* Filled triangle, optionally with rounded corners (radius px). */
void core_draw_triangle(Core_Renderer *r, vec2 a, vec2 b, vec2 c, vec4 color);
void core_draw_triangle_rounded(Core_Renderer *r, vec2 a, vec2 b, vec2 c, f32 radius, vec4 color);

/* Textured rectangle. `uv0`/`uv1` are the top-left/bottom-right UVs. */
void core_draw_image(Core_Renderer *r, Core_Texture tex, vec2 pos, vec2 size, vec4 tint);
void core_draw_image_uv(Core_Renderer *r, Core_Texture tex, vec2 pos, vec2 size,
                        vec2 uv0, vec2 uv1, vec4 tint);
/* Image masked to a rounded box; `lod` >= 0 forces a mip level (blur look),
   negative uses normal filtering. */
void core_draw_image_rounded(Core_Renderer *r, Core_Texture tex, vec2 pos, vec2 size,
                             vec2 uv0, vec2 uv1, f32 radius, f32 lod, vec4 tint);

/* Glyph quad from a single-channel coverage atlas (used by the text layer). */
void core_draw_glyph(Core_Renderer *r, Core_Texture tex, vec2 pos, vec2 size,
                     vec2 uv0, vec2 uv1, vec4 color);

/* ---- custom effects ----
 *
 * A full-quad fragment program for things the über-shader can't express
 * (backgrounds, visualizers). `frag_body` is GLSL 4.6 appended after this
 * prelude:
 *
 *   in vec2 v_px;          // pixel position in window space
 *   in vec2 v_uv;          // 0..1 across the quad
 *   uniform vec4  u_rect;  // x, y, w, h of the quad in px
 *   uniform vec2  u_res;   // framebuffer size
 *   uniform float u_time;
 *   uniform vec4  u_p[32]; // effect parameters
 *   uniform sampler2D u_tex0, u_tex1;
 *   out vec4 frag;         // write PREMULTIPLIED color
 *
 * Drawing an effect flushes the current batch (keeps ordering).
 */
typedef struct Core_Effect Core_Effect;
Core_Effect *core_effect_create(Core_Renderer *r, const char *name, const char *frag_body);
void core_draw_effect(Core_Renderer *r, Core_Effect *e, vec2 pos, vec2 size, f32 time,
                      const vec4 *params, u32 param_count,
                      Core_Texture tex0, Core_Texture tex1);

#endif /* CORE_RENDERER_H */
