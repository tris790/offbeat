/*
 * OpenGL 4.6 backend for the renderer abstraction declared in renderer.h.
 *
 * Game code never calls OpenGL (or this file) directly: it calls the
 * core_draw_* / core_renderer_* functions in renderer.h, and exactly one
 * backend .c implements them.
 *
 * Drawing strategy: every primitive is ONE instance (116 bytes) in a CPU
 * array. The vertex shader expands each instance into a quad from
 * gl_VertexID; the fragment shader evaluates the shape analytically (signed
 * distance fields for rounded boxes, circles, arcs, capsules, triangles) so
 * edges are antialiased at any size with no tessellation. Instances sharing
 * up to 4 textures go out in a single instanced draw call.
 */

#include "renderer.h"
#include "gl.h"

#include <stdio.h>
#include <string.h>

/* Shape kinds, matched in the fragment shader. */
enum {
    SHAPE_RECT     = 0, /* p0.x: gradient mode                               */
    SHAPE_BOX      = 1, /* p0 = (hw, hh, radius, border) p1 = (blur, grad)   */
    SHAPE_CIRCLE   = 2, /* p0 = (radius, stroke, a0, sweep) p1.x = blur      */
    SHAPE_IMAGE    = 3, /* p0 = (hw, hh, radius, lod)                        */
    SHAPE_GLYPH    = 4, /* coverage atlas                                    */
    SHAPE_LINE     = 5, /* p0 = (ax, ay, bx, by) p1 = (thickness, blur)      */
    SHAPE_TRIANGLE = 6, /* p0 = (ax, ay, bx, by) p1 = (cx, cy, radius)       */
};

#define FLAG_ADDITIVE (1u << 12)

typedef struct {
    f32 rect[4];   /* quad x0, y0, x1, y1 (px)        */
    f32 uv[4];     /* u0, v0, u1, v1                  */
    f32 color[4];
    f32 color2[4];
    f32 p0[4];
    f32 p1[4];
    f32 clip[4];   /* x0, y0, x1, y1                  */
    u32 kind;      /* kind | slot << 8 | flags        */
} Instance;

#define MAX_INSTANCES (16 * 1024)
#define MAX_SLOTS     4
#define MAX_CLIPS     32

struct Core_Effect {
    u32 program;
    s32 u_rect, u_res, u_time, u_p, u_tex0, u_tex1;
};

struct Core_Renderer {
    Core_Arena *arena;

    u32 vao, vbo;
    u32 program;
    s32 u_res;
    s32 u_tex[MAX_SLOTS];

    u32 effect_vao;

    u32 width, height;

    Instance *inst;
    u32 inst_count;

    u32 slot_tex[MAX_SLOTS]; /* GL texture per slot in the current batch */
    u32 slot_count;

    f32 clips[MAX_CLIPS][4];
    u32 clip_depth;

    u32 blend_flags;

    f32 cm[9];     /* color matrix (row-major), applied when cm_on */
    b32 cm_on;

    u32 fbo, fbo_rb, fbo_w, fbo_h;

    Core_RenderStats stats, stats_last;
};

/* ---- shaders ---- */

static const char *VERT_SRC =
    "#version 460 core\n"
    "layout(location=0) in vec4 a_rect;\n"
    "layout(location=1) in vec4 a_uv;\n"
    "layout(location=2) in vec4 a_color;\n"
    "layout(location=3) in vec4 a_color2;\n"
    "layout(location=4) in vec4 a_p0;\n"
    "layout(location=5) in vec4 a_p1;\n"
    "layout(location=6) in vec4 a_clip;\n"
    "layout(location=7) in uint a_kind;\n"
    "uniform vec2 u_res;\n"
    "out vec2 v_px;\n"
    "out vec2 v_uv;\n"
    "flat out vec4 v_rect, v_color, v_color2, v_p0, v_p1, v_clip;\n"
    "flat out uint v_kind;\n"
    "void main(){\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 pos = mix(a_rect.xy, a_rect.zw, c);\n"
    "  v_px = pos; v_uv = mix(a_uv.xy, a_uv.zw, c);\n"
    "  v_rect = a_rect; v_color = a_color; v_color2 = a_color2;\n"
    "  v_p0 = a_p0; v_p1 = a_p1; v_clip = a_clip; v_kind = a_kind;\n"
    "  vec2 ndc = pos / u_res * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);\n"
    "}\n";

static const char *FRAG_SRC =
    "#version 460 core\n"
    "in vec2 v_px;\n"
    "in vec2 v_uv;\n"
    "flat in vec4 v_rect, v_color, v_color2, v_p0, v_p1, v_clip;\n"
    "flat in uint v_kind;\n"
    "uniform sampler2D u_tex0, u_tex1, u_tex2, u_tex3;\n"
    "out vec4 frag;\n"
    "const float TAU = 6.28318530718;\n"
    "vec4 samp(uint slot, vec2 uv, float lod){\n"
    "  if (lod >= 0.0) {\n"
    "    if (slot == 0u) return textureLod(u_tex0, uv, lod);\n"
    "    if (slot == 1u) return textureLod(u_tex1, uv, lod);\n"
    "    if (slot == 2u) return textureLod(u_tex2, uv, lod);\n"
    "    return textureLod(u_tex3, uv, lod);\n"
    "  }\n"
    "  if (slot == 0u) return texture(u_tex0, uv);\n"
    "  if (slot == 1u) return texture(u_tex1, uv);\n"
    "  if (slot == 2u) return texture(u_tex2, uv);\n"
    "  return texture(u_tex3, uv);\n"
    "}\n"
    "float sd_box(vec2 p, vec2 b, float r){\n"
    "  vec2 q = abs(p) - b + r;\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "}\n"
    "float sd_tri(vec2 p, vec2 p0, vec2 p1, vec2 p2){\n"
    "  vec2 e0 = p1-p0, e1 = p2-p1, e2 = p0-p2;\n"
    "  vec2 v0 = p-p0, v1 = p-p1, v2 = p-p2;\n"
    "  vec2 pq0 = v0 - e0*clamp(dot(v0,e0)/dot(e0,e0), 0.0, 1.0);\n"
    "  vec2 pq1 = v1 - e1*clamp(dot(v1,e1)/dot(e1,e1), 0.0, 1.0);\n"
    "  vec2 pq2 = v2 - e2*clamp(dot(v2,e2)/dot(e2,e2), 0.0, 1.0);\n"
    "  float s = sign(e0.x*e2.y - e0.y*e2.x);\n"
    "  vec2 d = min(min(vec2(dot(pq0,pq0), s*(v0.x*e0.y-v0.y*e0.x)),\n"
    "                   vec2(dot(pq1,pq1), s*(v1.x*e1.y-v1.y*e1.x))),\n"
    "                   vec2(dot(pq2,pq2), s*(v2.x*e2.y-v2.y*e2.x)));\n"
    "  return -sqrt(d.x)*sign(d.y);\n"
    "}\n"
    "float cover(float d, float blur){\n"
    "  return blur > 0.0 ? 1.0 - smoothstep(-blur, blur, d) : clamp(0.5 - d, 0.0, 1.0);\n"
    "}\n"
    "void main(){\n"
    "  if (v_px.x < v_clip.x || v_px.y < v_clip.y || v_px.x >= v_clip.z || v_px.y >= v_clip.w) discard;\n"
    "  uint kind = v_kind & 255u;\n"
    "  uint slot = (v_kind >> 8) & 15u;\n"
    "  bool additive = (v_kind & 4096u) != 0u;\n"
    "  vec4 c = v_color;\n"
    "  float a = 1.0;\n"
    "  vec2 p = v_px - (v_rect.xy + v_rect.zw) * 0.5;\n"
    "  if (kind == 0u) {\n"
    "    if (v_p0.x == 1.0) c = mix(v_color, v_color2, v_uv.y);\n"
    "    else if (v_p0.x == 2.0) c = mix(v_color, v_color2, v_uv.x);\n"
    "  } else if (kind == 1u) {\n"
    "    float d = sd_box(p, v_p0.xy, v_p0.z);\n"
    "    if (v_p1.y == 1.0) c = mix(v_color, v_color2, v_uv.y);\n"
    "    else if (v_p1.y == 2.0) c = mix(v_color, v_color2, v_uv.x);\n"
    "    if (v_p0.w > 0.0) {\n"
    "      float inner = clamp(0.5 - (d + v_p0.w), 0.0, 1.0);\n"
    "      vec4 fill = c; vec4 border = v_color2;\n"
    "      float fa = fill.a * inner; float ba = border.a * (1.0 - inner);\n"
    "      float oa = fa + ba;\n"
    "      c = vec4(oa > 0.0 ? (fill.rgb * fa + border.rgb * ba) / oa : fill.rgb, oa);\n"
    "    }\n"
    "    a = cover(d, v_p1.x);\n"
    "  } else if (kind == 2u) {\n"
    "    float r = v_p0.x, stroke = v_p0.y, a0 = v_p0.z, sweep = v_p0.w;\n"
    "    float len = length(p);\n"
    "    float d;\n"
    "    if (stroke <= 0.0) d = len - r;\n"
    "    else if (sweep <= 0.0 || sweep >= TAU) d = abs(len - r) - stroke * 0.5;\n"
    "    else {\n"
    "      float rel = mod(atan(p.y, p.x) - a0, TAU);\n"
    "      if (rel <= sweep) d = abs(len - r) - stroke * 0.5;\n"
    "      else {\n"
    "        vec2 e0 = r * vec2(cos(a0), sin(a0));\n"
    "        vec2 e1 = r * vec2(cos(a0 + sweep), sin(a0 + sweep));\n"
    "        d = min(length(p - e0), length(p - e1)) - stroke * 0.5;\n"
    "      }\n"
    "    }\n"
    "    a = cover(d, v_p1.x);\n"
    "  } else if (kind == 3u) {\n"
    "    c = samp(slot, v_uv, v_p0.w) * v_color;\n"
    "    if (v_p0.z > 0.0) a = cover(sd_box(p, v_p0.xy, v_p0.z), 0.0);\n"
    "  } else if (kind == 4u) {\n"
    "    float cov = samp(slot, v_uv, -1.0).r;\n"
    "    a = pow(cov, 0.85);\n"
    "  } else if (kind == 5u) {\n"
    "    vec2 pa = v_px - v_p0.xy, ba = v_p0.zw - v_p0.xy;\n"
    "    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);\n"
    "    float d = length(pa - ba * h) - v_p1.x * 0.5;\n"
    "    c = mix(v_color, v_color2, h);\n"
    "    a = cover(d, v_p1.y);\n"
    "  } else {\n"
    "    float d = sd_tri(v_px, v_p0.xy, v_p0.zw, v_p1.xy) - v_p1.z;\n"
    "    a = cover(d, 0.0);\n"
    "  }\n"
    "  a *= c.a;\n"
    "  if (a <= 0.0) discard;\n"
    "  frag = vec4(c.rgb * a, additive ? 0.0 : a);\n"
    "}\n";

static const char *EFFECT_VERT_SRC =
    "#version 460 core\n"
    "uniform vec4 u_rect;\n"
    "uniform vec2 u_res;\n"
    "out vec2 v_px;\n"
    "out vec2 v_uv;\n"
    "void main(){\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 pos = u_rect.xy + c * u_rect.zw;\n"
    "  v_px = pos; v_uv = c;\n"
    "  vec2 ndc = pos / u_res * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);\n"
    "}\n";

static const char *EFFECT_PRELUDE =
    "#version 460 core\n"
    "in vec2 v_px;\n"
    "in vec2 v_uv;\n"
    "uniform vec4 u_rect;\n"
    "uniform vec2 u_res;\n"
    "uniform float u_time;\n"
    "uniform vec4 u_p[32];\n"
    "uniform sampler2D u_tex0, u_tex1;\n"
    "out vec4 frag;\n"
    "#line 1\n";

static u32 compile_shader(GLenum type, const char *const *srcs, u32 count, const char *name) {
    u32 sh = glCreateShader(type);
    glShaderSource(sh, (GLsizei)count, srcs, 0);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), 0, log);
        fprintf(stderr, "[renderer] shader '%s' compile failed:\n%s\n", name, log);
    }
    return sh;
}

static u32 link_program(const char *vs, const char *const *fs, u32 fs_count, const char *name) {
    u32 v = compile_shader(GL_VERTEX_SHADER, &vs, 1, name);
    u32 f = compile_shader(GL_FRAGMENT_SHADER, fs, fs_count, name);
    u32 p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), 0, log);
        fprintf(stderr, "[renderer] program '%s' link failed:\n%s\n", name, log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

/* ---- lifecycle ---- */

Core_Renderer *core_renderer_create(Core_Arena *arena, Core_GlLoadProc load_proc) {
    if (!core_gl_load(load_proc)) {
        fprintf(stderr, "[renderer] failed to load required GL functions\n");
        return 0;
    }

    Core_Renderer *r = core_push_struct(arena, Core_Renderer);
    r->arena = arena;
    r->inst = core_push_array(arena, Instance, MAX_INSTANCES);

    r->program = link_program(VERT_SRC, &FRAG_SRC, 1, "ui");
    r->u_res = glGetUniformLocation(r->program, "u_res");
    static const char *tex_names[MAX_SLOTS] = { "u_tex0", "u_tex1", "u_tex2", "u_tex3" };
    for (u32 i = 0; i < MAX_SLOTS; i++) r->u_tex[i] = glGetUniformLocation(r->program, tex_names[i]);

    glGenVertexArrays(1, &r->vao);
    glBindVertexArray(r->vao);
    glGenBuffers(1, &r->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(Instance) * MAX_INSTANCES, 0, GL_STREAM_DRAW);

    #define ATTR(loc, member) \
        glEnableVertexAttribArray(loc); \
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), \
                              (const void *)(uintptr_t)offsetof(Instance, member)); \
        glVertexAttribDivisor(loc, 1)
    ATTR(0, rect);
    ATTR(1, uv);
    ATTR(2, color);
    ATTR(3, color2);
    ATTR(4, p0);
    ATTR(5, p1);
    ATTR(6, clip);
    #undef ATTR
    glEnableVertexAttribArray(7);
    glVertexAttribIPointer(7, 1, GL_UNSIGNED_INT, sizeof(Instance),
                           (const void *)(uintptr_t)offsetof(Instance, kind));
    glVertexAttribDivisor(7, 1);
    glBindVertexArray(0);

    glGenVertexArrays(1, &r->effect_vao);

    glEnable(GL_BLEND);
    /* Shaders output premultiplied color. */
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    return r;
}

void core_renderer_destroy(Core_Renderer *r) {
    if (!r) return;
    glDeleteProgram(r->program);
    glDeleteBuffers(1, &r->vbo);
    glDeleteVertexArrays(1, &r->vao);
    glDeleteVertexArrays(1, &r->effect_vao);
    if (r->fbo) glDeleteFramebuffers(1, &r->fbo);
    if (r->fbo_rb) glDeleteRenderbuffers(1, &r->fbo_rb);
}

void core_renderer_resize(Core_Renderer *r, u32 width, u32 height) {
    r->width = width;
    r->height = height;
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
}

b32 core_renderer_use_offscreen(Core_Renderer *r, u32 width, u32 height) {
    if (r->fbo && r->fbo_w == width && r->fbo_h == height) return true;
    if (!r->fbo) {
        glGenFramebuffers(1, &r->fbo);
        glGenRenderbuffers(1, &r->fbo_rb);
    }
    glBindRenderbuffer(GL_RENDERBUFFER, r->fbo_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, (GLsizei)width, (GLsizei)height);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, r->fbo_rb);
    r->fbo_w = width;
    r->fbo_h = height;
    b32 ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    core_renderer_resize(r, width, height);
    return ok;
}

void core_renderer_read_pixels(Core_Renderer *r, u32 width, u32 height, void *out_rgba) {
    CORE_UNUSED(r);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);
    /* GL rows are bottom-up; flip in place. */
    u8 *px = out_rgba;
    u32 stride = width * 4;
    for (u32 y = 0; y < height / 2; y++) {
        u8 *a = px + y * stride, *b = px + (height - 1 - y) * stride;
        for (u32 i = 0; i < stride; i++) { u8 t = a[i]; a[i] = b[i]; b[i] = t; }
    }
}

Core_RenderStats core_renderer_stats(Core_Renderer *r) { return r->stats_last; }

/* ---- batching ---- */

static void flush(Core_Renderer *r) {
    if (r->inst_count == 0) return;

    glUseProgram(r->program);
    glUniform2f(r->u_res, (f32)r->width, (f32)r->height);
    for (u32 i = 0; i < MAX_SLOTS; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, i < r->slot_count ? r->slot_tex[i] : 0);
        glUniform1i(r->u_tex[i], (GLint)i);
    }

    glBindVertexArray(r->vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    /* Orphan the buffer so the driver never stalls on the previous draw. */
    glBufferData(GL_ARRAY_BUFFER, sizeof(Instance) * MAX_INSTANCES, 0, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(Instance) * r->inst_count, r->inst);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)r->inst_count);

    r->stats.draw_calls++;
    r->stats.instances += r->inst_count;
    r->inst_count = 0;
    r->slot_count = 0;
}

/* Slot for `tex` in the current batch, flushing if all slots are taken. */
static u32 tex_slot(Core_Renderer *r, u32 tex) {
    for (u32 i = 0; i < r->slot_count; i++)
        if (r->slot_tex[i] == tex) return i;
    if (r->slot_count == MAX_SLOTS) flush(r);
    r->slot_tex[r->slot_count] = tex;
    return r->slot_count++;
}

static Instance *push(Core_Renderer *r, u32 kind, u32 tex,
                      f32 x0, f32 y0, f32 x1, f32 y1) {
    u32 slot = tex ? tex_slot(r, tex) : 0;
    if (r->inst_count == MAX_INSTANCES) {
        flush(r);
        if (tex) slot = tex_slot(r, tex);
    }
    Instance *in = &r->inst[r->inst_count++];
    in->rect[0] = x0; in->rect[1] = y0; in->rect[2] = x1; in->rect[3] = y1;
    in->uv[0] = 0; in->uv[1] = 0; in->uv[2] = 1; in->uv[3] = 1;
    memcpy(in->clip, r->clips[r->clip_depth], sizeof(in->clip));
    in->kind = kind | (slot << 8) | r->blend_flags;
    memset(in->p0, 0, sizeof(in->p0));
    memset(in->p1, 0, sizeof(in->p1));
    memset(in->color2, 0, sizeof(in->color2));
    return in;
}

CORE_INLINE void set4(f32 *dst, f32 a, f32 b, f32 c, f32 d) {
    dst[0] = a; dst[1] = b; dst[2] = c; dst[3] = d;
}
CORE_INLINE void setc(f32 *dst, vec4 c) { set4(dst, c.r, c.g, c.b, c.a); }

/* Shape/text color through the active color matrix (images are exempt). */
static void setcol(Core_Renderer *r, f32 *dst, vec4 c) {
    if (r->cm_on) {
        vec3 rgb = core_color_matrix_apply(r->cm, vec3_make(c.r, c.g, c.b));
        c.r = rgb.r; c.g = rgb.g; c.b = rgb.b;
    }
    setc(dst, c);
}

/* ---- frame ---- */

void core_renderer_begin_frame(Core_Renderer *r, vec4 clear) {
    if (r->fbo) glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glClearColor(clear.r * clear.a, clear.g * clear.a, clear.b * clear.a, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);
    r->inst_count = 0;
    r->slot_count = 0;
    r->clip_depth = 0;
    set4(r->clips[0], -1e9f, -1e9f, 1e9f, 1e9f);
    r->blend_flags = 0;
    r->stats = (Core_RenderStats){0};
}

void core_renderer_end_frame(Core_Renderer *r) {
    flush(r);
    r->stats_last = r->stats;
}

void core_renderer_mask_rounded(Core_Renderer *r, vec2 pos, vec2 size, f32 radius) {
    flush(r);
    glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
    core_draw_rect_rounded(r, pos, size, radius, (vec4){ .x = 1, .y = 1, .z = 1, .w = 1 });
    flush(r);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    r->stats_last = r->stats;
}

/* ---- clipping / blend ---- */

void core_clip_push(Core_Renderer *r, vec2 pos, vec2 size) {
    if (r->clip_depth + 1 >= MAX_CLIPS) return;
    f32 *cur = r->clips[r->clip_depth];
    f32 *nxt = r->clips[++r->clip_depth];
    nxt[0] = CORE_MAX(cur[0], pos.x);
    nxt[1] = CORE_MAX(cur[1], pos.y);
    nxt[2] = CORE_MIN(cur[2], pos.x + size.x);
    nxt[3] = CORE_MIN(cur[3], pos.y + size.y);
}

void core_clip_pop(Core_Renderer *r) {
    if (r->clip_depth > 0) r->clip_depth--;
}

void core_set_color_matrix(Core_Renderer *r, const f32 *m) {
    r->cm_on = m != 0;
    if (m) memcpy(r->cm, m, sizeof(r->cm));
}

void core_set_blend(Core_Renderer *r, Core_Blend blend) {
    r->blend_flags = (blend == CORE_BLEND_ADD) ? FLAG_ADDITIVE : 0;
}

/* ---- textures ---- */

static u32 mip_count(u32 w, u32 h) {
    u32 n = 1, m = CORE_MAX(w, h);
    while (m > 1) { m >>= 1; n++; }
    return n;
}

Core_Texture core_texture_create(Core_Renderer *r, u32 format, u32 w, u32 h,
                                 const void *pixels, b32 mipmapped) {
    CORE_UNUSED(r);
    u32 id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    u32 levels = mipmapped ? mip_count(w, h) : 1;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    b32 single = (format == CORE_TEXTURE_R8);
    GLenum internal = single ? GL_R8 : GL_RGBA8;
    GLenum fmt = single ? GL_RED : GL_RGBA;
    /* Allocate every level so the texture is complete even before mips are
       generated. */
    u32 lw = w, lh = h;
    for (u32 level = 0; level < levels; level++) {
        glTexImage2D(GL_TEXTURE_2D, (GLint)level, (GLint)internal, (GLsizei)lw, (GLsizei)lh, 0,
                     fmt, GL_UNSIGNED_BYTE, level == 0 ? pixels : 0);
        lw = CORE_MAX(1u, lw / 2);
        lh = CORE_MAX(1u, lh / 2);
    }
    if (mipmapped && pixels) glGenerateMipmap(GL_TEXTURE_2D);
    return (Core_Texture){ .id = id, .width = w, .height = h, .format = format, .mip_levels = levels };
}

Core_Texture core_texture_create_rgba(Core_Renderer *r, u32 w, u32 h, const void *pixels) {
    return core_texture_create(r, CORE_TEXTURE_RGBA8, w, h, pixels, false);
}

Core_Texture core_texture_create_r8(Core_Renderer *r, u32 w, u32 h, const void *pixels) {
    return core_texture_create(r, CORE_TEXTURE_R8, w, h, pixels, false);
}

void core_texture_update(Core_Renderer *r, Core_Texture tex,
                         u32 x, u32 y, u32 w, u32 h, const void *pixels) {
    /* A texture referenced by pending instances must not change under them. */
    for (u32 i = 0; i < r->slot_count; i++)
        if (r->slot_tex[i] == tex.id) { flush(r); break; }
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    GLenum fmt = tex.format == CORE_TEXTURE_R8 ? GL_RED : GL_RGBA;
    glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h,
                    fmt, GL_UNSIGNED_BYTE, pixels);
}

void core_texture_generate_mips(Core_Renderer *r, Core_Texture tex) {
    CORE_UNUSED(r);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glGenerateMipmap(GL_TEXTURE_2D);
}

void core_texture_destroy(Core_Renderer *r, Core_Texture tex) {
    if (!tex.id) return;
    for (u32 i = 0; i < r->slot_count; i++)
        if (r->slot_tex[i] == tex.id) { flush(r); break; }
    glDeleteTextures(1, &tex.id);
}

/* ---- primitives ---- */

void core_draw_rect(Core_Renderer *r, vec2 pos, vec2 size, vec4 color) {
    Instance *in = push(r, SHAPE_RECT, 0, pos.x, pos.y, pos.x + size.x, pos.y + size.y);
    setcol(r, in->color, color);
}

void core_draw_rect_gradient(Core_Renderer *r, vec2 pos, vec2 size, vec4 c0, vec4 c1, b32 horizontal) {
    Instance *in = push(r, SHAPE_RECT, 0, pos.x, pos.y, pos.x + size.x, pos.y + size.y);
    setcol(r, in->color, c0);
    setcol(r, in->color2, c1);
    in->p0[0] = horizontal ? 2.0f : 1.0f;
}

void core_draw_box(Core_Renderer *r, vec2 pos, vec2 size, const Core_BoxStyle *s) {
    f32 hw = size.x * 0.5f, hh = size.y * 0.5f;
    f32 rad = CORE_MIN(s->radius, CORE_MIN(hw, hh));
    Instance *in = push(r, SHAPE_BOX, 0, pos.x - 1, pos.y - 1, pos.x + size.x + 1, pos.y + size.y + 1);
    setcol(r, in->color, s->fill);
    if (s->border > 0) {
        setcol(r, in->color2, s->border_color);
    } else if (s->gradient) {
        setcol(r, in->color2, s->fill2);
        in->p1[1] = (f32)s->gradient;
    }
    set4(in->p0, hw, hh, rad, s->border);
    /* uv spans the padded quad; remap so gradients run edge to edge */
    in->uv[0] = -1.0f / size.x; in->uv[1] = -1.0f / size.y;
    in->uv[2] = 1.0f + 1.0f / size.x; in->uv[3] = 1.0f + 1.0f / size.y;
}

void core_draw_rect_rounded(Core_Renderer *r, vec2 pos, vec2 size, f32 radius, vec4 color) {
    Core_BoxStyle s = { .radius = radius, .fill = color };
    core_draw_box(r, pos, size, &s);
}

void core_draw_shadow(Core_Renderer *r, vec2 pos, vec2 size, f32 radius, f32 blur, vec4 color) {
    f32 hw = size.x * 0.5f, hh = size.y * 0.5f;
    f32 rad = CORE_MIN(radius, CORE_MIN(hw, hh));
    f32 pad = blur + 1;
    Instance *in = push(r, SHAPE_BOX, 0, pos.x - pad, pos.y - pad,
                        pos.x + size.x + pad, pos.y + size.y + pad);
    setcol(r, in->color, color);
    set4(in->p0, hw, hh, rad, 0);
    in->p1[0] = CORE_MAX(blur, 0.001f);
}

void core_draw_circle_ex(Core_Renderer *r, vec2 c, f32 radius, f32 stroke, f32 blur, vec4 color) {
    f32 ext = radius + (stroke > 0 ? stroke * 0.5f : 0) + blur + 1;
    Instance *in = push(r, SHAPE_CIRCLE, 0, c.x - ext, c.y - ext, c.x + ext, c.y + ext);
    setcol(r, in->color, color);
    set4(in->p0, radius, stroke, 0, 0);
    in->p1[0] = blur;
}

void core_draw_circle(Core_Renderer *r, vec2 center, f32 radius, vec4 color) {
    core_draw_circle_ex(r, center, radius, 0, 0, color);
}

void core_draw_arc(Core_Renderer *r, vec2 c, f32 radius, f32 stroke,
                   f32 a0, f32 sweep, vec4 color) {
    if (sweep <= 0.0001f) return;
    f32 ext = radius + stroke * 0.5f + 1;
    Instance *in = push(r, SHAPE_CIRCLE, 0, c.x - ext, c.y - ext, c.x + ext, c.y + ext);
    setcol(r, in->color, color);
    /* normalize a0 into [0, tau) for the shader's mod */
    a0 = core_wrap(a0, CORE_TAU);
    set4(in->p0, radius, CORE_MAX(stroke, 0.01f), a0, CORE_MIN(sweep, CORE_TAU));
}

void core_draw_line_ex(Core_Renderer *r, vec2 a, vec2 b, f32 thickness, f32 blur, vec4 c0, vec4 c1) {
    f32 pad = thickness * 0.5f + blur + 1;
    Instance *in = push(r, SHAPE_LINE, 0,
                        CORE_MIN(a.x, b.x) - pad, CORE_MIN(a.y, b.y) - pad,
                        CORE_MAX(a.x, b.x) + pad, CORE_MAX(a.y, b.y) + pad);
    setcol(r, in->color, c0);
    setcol(r, in->color2, c1);
    set4(in->p0, a.x, a.y, b.x, b.y);
    in->p1[0] = thickness;
    in->p1[1] = blur;
}

void core_draw_line(Core_Renderer *r, vec2 a, vec2 b, f32 thickness, vec4 color) {
    core_draw_line_ex(r, a, b, thickness, 0, color, color);
}

void core_draw_triangle_rounded(Core_Renderer *r, vec2 a, vec2 b, vec2 c, f32 radius, vec4 color) {
    f32 pad = radius + 1;
    f32 x0 = CORE_MIN(a.x, CORE_MIN(b.x, c.x)) - pad, y0 = CORE_MIN(a.y, CORE_MIN(b.y, c.y)) - pad;
    f32 x1 = CORE_MAX(a.x, CORE_MAX(b.x, c.x)) + pad, y1 = CORE_MAX(a.y, CORE_MAX(b.y, c.y)) + pad;
    Instance *in = push(r, SHAPE_TRIANGLE, 0, x0, y0, x1, y1);
    setcol(r, in->color, color);
    set4(in->p0, a.x, a.y, b.x, b.y);
    set4(in->p1, c.x, c.y, radius, 0);
}

void core_draw_triangle(Core_Renderer *r, vec2 a, vec2 b, vec2 c, vec4 color) {
    core_draw_triangle_rounded(r, a, b, c, 0, color);
}

void core_draw_image_rounded(Core_Renderer *r, Core_Texture tex, vec2 pos, vec2 size,
                             vec2 uv0, vec2 uv1, f32 radius, f32 lod, vec4 tint) {
    if (!tex.id) return;
    Instance *in = push(r, SHAPE_IMAGE, tex.id, pos.x, pos.y, pos.x + size.x, pos.y + size.y);
    set4(in->uv, uv0.x, uv0.y, uv1.x, uv1.y);
    setc(in->color, tint);
    f32 hw = size.x * 0.5f, hh = size.y * 0.5f;
    set4(in->p0, hw, hh, CORE_MIN(radius, CORE_MIN(hw, hh)), lod);
}

void core_draw_image_uv(Core_Renderer *r, Core_Texture tex, vec2 pos, vec2 size,
                        vec2 uv0, vec2 uv1, vec4 tint) {
    core_draw_image_rounded(r, tex, pos, size, uv0, uv1, 0, -1, tint);
}

void core_draw_image(Core_Renderer *r, Core_Texture tex, vec2 pos, vec2 size, vec4 tint) {
    core_draw_image_rounded(r, tex, pos, size, vec2_make(0, 0), vec2_make(1, 1), 0, -1, tint);
}

void core_draw_glyph(Core_Renderer *r, Core_Texture tex, vec2 pos, vec2 size,
                     vec2 uv0, vec2 uv1, vec4 color) {
    Instance *in = push(r, SHAPE_GLYPH, tex.id, pos.x, pos.y, pos.x + size.x, pos.y + size.y);
    set4(in->uv, uv0.x, uv0.y, uv1.x, uv1.y);
    setcol(r, in->color, color);
}

/* ---- effects ---- */

Core_Effect *core_effect_create(Core_Renderer *r, const char *name, const char *frag_body) {
    Core_Effect *e = core_push_struct(r->arena, Core_Effect);
    const char *parts[2] = { EFFECT_PRELUDE, frag_body };
    e->program = link_program(EFFECT_VERT_SRC, parts, 2, name);
    e->u_rect = glGetUniformLocation(e->program, "u_rect");
    e->u_res  = glGetUniformLocation(e->program, "u_res");
    e->u_time = glGetUniformLocation(e->program, "u_time");
    e->u_p    = glGetUniformLocation(e->program, "u_p");
    e->u_tex0 = glGetUniformLocation(e->program, "u_tex0");
    e->u_tex1 = glGetUniformLocation(e->program, "u_tex1");
    return e;
}

void core_draw_effect(Core_Renderer *r, Core_Effect *e, vec2 pos, vec2 size, f32 time,
                      const vec4 *params, u32 param_count,
                      Core_Texture tex0, Core_Texture tex1) {
    flush(r);
    glUseProgram(e->program);
    glUniform4f(e->u_rect, pos.x, pos.y, size.x, size.y);
    glUniform2f(e->u_res, (f32)r->width, (f32)r->height);
    glUniform1f(e->u_time, time);
    if (param_count && e->u_p >= 0) glUniform4fv(e->u_p, (GLsizei)CORE_MIN(param_count, 32u), (const f32 *)params);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex0.id);
    glUniform1i(e->u_tex0, 0);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, tex1.id);
    glUniform1i(e->u_tex1, 1);
    glBindVertexArray(r->effect_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    r->stats.draw_calls++;
}
