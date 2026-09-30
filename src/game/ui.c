#include "ui.h"

#include <math.h>
#include <string.h>

#define ANIM_CAP 4096 /* power of two */

const vec4 UI_BG            = UI_RGBA_C(14, 12, 24, 1.0f);
const vec4 UI_PANEL         = UI_RGBA_C(17, 15, 29, 0.86f);
const vec4 UI_TEXT          = UI_RGBA_C(240, 238, 250, 1.0f);
const vec4 UI_TEXT_DIM      = UI_RGBA_C(146, 141, 170, 1.0f);
const vec4 UI_TEXT_FAINT    = UI_RGBA_C(96, 92, 118, 1.0f);
const vec4 UI_ACCENT        = UI_RGBA_C(139, 92, 246, 1.0f);
const vec4 UI_ACCENT_BRIGHT = UI_RGBA_C(171, 140, 255, 1.0f);
const vec4 UI_LAVENDER      = UI_RGBA_C(186, 170, 255, 1.0f);
const vec4 UI_LINE          = UI_RGBA_C(255, 255, 255, 0.07f);

vec4 ui_alpha(vec4 c, f32 a) { c.a *= a; return c; }
vec4 ui_rgb(vec3 c, f32 a) { return (vec4){ .x = c.x, .y = c.y, .z = c.z, .w = a }; }
vec4 ui_mix(vec4 a, vec4 b, f32 t) {
    return (vec4){ .x = core_lerp(a.x, b.x, t), .y = core_lerp(a.y, b.y, t),
                   .z = core_lerp(a.z, b.z, t), .w = core_lerp(a.w, b.w, t) };
}

/* ---- ids ---- */

u64 ui_id(const char *name) {
    u64 h = 0xcbf29ce484222325ULL;
    for (const u8 *p = (const u8 *)name; *p; p++) { h ^= *p; h *= 0x100000001b3ULL; }
    return h | 1;
}

u64 ui_idx(u64 base, u64 index) {
    u64 h = base ^ (index + 0x9e3779b97f4a7c15ULL + (base << 6) + (base >> 2));
    h ^= h >> 31; h *= 0xbf58476d1ce4e5b9ULL; h ^= h >> 29;
    return h | 1;
}

/* ---- frame ---- */

void ui_init(Ui *ui, Core_Arena *arena, Core_Renderer *r, Core_Text *text) {
    memset(ui, 0, sizeof(*ui));
    ui->r = r;
    ui->text = text;
    ui->anim_cap = ANIM_CAP;
    ui->anims = core_push_array(arena, Ui_Anim, ANIM_CAP);
    ui->scale = 1;
}

void ui_begin(Ui *ui, const Platform_Input *in, f32 dt, vec2 size, f32 scale) {
    ui->in = in;
    ui->dt = dt;
    ui->time += dt;
    ui->frame++;
    ui->size = size;
    ui->scale = scale;
    ui->hot = 0;
    ui->mouse_taken = false;
    ui->input_enabled = true;
    ui->cursor = PLATFORM_CURSOR_DEFAULT;
    ui->animating = false;

    ui->double_click = false;
    if (in->mouse_pressed[PLATFORM_MOUSE_LEFT]) {
        f64 now = ui->time;
        if (now - ui->last_click_time < 0.35 &&
            vec2_distance(in->mouse_pos, ui->last_click_pos) < 6) {
            ui->double_click = true;
            ui->last_click_time = 0;
        } else {
            ui->last_click_time = now;
        }
        ui->last_click_pos = in->mouse_pos;
    }
    if (!in->mouse_down[PLATFORM_MOUSE_LEFT] && !in->mouse_released[PLATFORM_MOUSE_LEFT])
        ui->active = 0;
}

void ui_end(Ui *ui) {
    f32 dt = ui->dt;
    /* particles: simple ballistic motion with drag and a little gravity */
    u32 w = 0;
    for (u32 i = 0; i < ui->particle_count; i++) {
        Ui_Particle p = ui->particles[i];
        p.life -= dt;
        if (p.life <= 0) continue;
        p.pos = vec2_add(p.pos, vec2_mul(p.vel, dt));
        f32 drag = expf(-dt * 4.0f);
        p.vel = vec2_mul(p.vel, drag);
        p.vel.y += 60.0f * dt;
        ui->particles[w++] = p;
    }
    ui->particle_count = w;

    w = 0;
    for (u32 i = 0; i < ui->ripple_count; i++) {
        Ui_Ripple rp = ui->ripples[i];
        rp.t += dt / 0.55f;
        if (rp.t >= 1) continue;
        ui->ripples[w++] = rp;
    }
    ui->ripple_count = w;
    if (ui->particle_count || ui->ripple_count) ui->animating = true;
}

/* ---- animation table ---- */

static Ui_Anim *anim_slot(Ui *ui, u64 id, f32 init, b32 *created) {
    u32 mask = ui->anim_cap - 1;
    u32 i = (u32)(id ^ (id >> 29)) & mask;
    Ui_Anim *stale = 0;
    for (u32 probe = 0; probe < 32; probe++) {
        Ui_Anim *a = &ui->anims[i];
        if (a->id == id) { a->touched = ui->frame; *created = false; return a; }
        if (a->id == 0) {
            a->id = id; a->value = init; a->velocity = 0; a->touched = ui->frame;
            *created = true;
            return a;
        }
        /* entries unused for a few seconds can be recycled */
        if (!stale && ui->frame - a->touched > 600) stale = a;
        i = (i + 1) & mask;
    }
    if (!stale) stale = &ui->anims[(u32)(id ^ (id >> 29)) & mask];
    stale->id = id; stale->value = init; stale->velocity = 0; stale->touched = ui->frame;
    *created = true;
    return stale;
}

f32 ui_ease(Ui *ui, u64 id, f32 target, f32 rate) {
    b32 created;
    Ui_Anim *a = anim_slot(ui, id, target, &created);
    if (created) return target;
    f32 k = 1.0f - expf(-rate * ui->dt);
    a->value += (target - a->value) * k;
    if (fabsf(target - a->value) < 0.0005f) a->value = target;
    else ui->animating = true;
    return a->value;
}

f32 ui_spring(Ui *ui, u64 id, f32 target, f32 stiffness, f32 damping) {
    b32 created;
    Ui_Anim *a = anim_slot(ui, id, target, &created);
    if (created) return target;
    /* semi-implicit Euler in small substeps for stability at low fps */
    f32 dt = ui->dt;
    u32 steps = (u32)ceilf(dt / (1.0f / 240.0f));
    if (steps < 1) steps = 1;
    if (steps > 16) steps = 16;
    f32 h = dt / (f32)steps;
    for (u32 s = 0; s < steps; s++) {
        f32 force = (target - a->value) * stiffness - a->velocity * damping;
        a->velocity += force * h;
        a->value += a->velocity * h;
    }
    if (fabsf(target - a->value) < 0.0005f && fabsf(a->velocity) < 0.005f) {
        a->value = target;
        a->velocity = 0;
    } else {
        ui->animating = true;
    }
    return a->value;
}

void ui_anim_set(Ui *ui, u64 id, f32 value) {
    b32 created;
    Ui_Anim *a = anim_slot(ui, id, value, &created);
    a->value = value;
    a->velocity = 0;
}

f32 ui_anim_get(Ui *ui, u64 id, f32 fallback) {
    b32 created;
    Ui_Anim *a = anim_slot(ui, id, fallback, &created);
    return a->value;
}

void ui_anim_kick(Ui *ui, u64 id, f32 velocity) {
    b32 created;
    Ui_Anim *a = anim_slot(ui, id, 0, &created);
    a->velocity += velocity;
    ui->animating = true;
}

/* ---- interaction ---- */

b32 ui_mouse_in(Ui *ui, vec2 pos, vec2 size) {
    if (!ui->input_enabled || !ui->in->mouse_inside) return false;
    vec2 m = ui->in->mouse_pos;
    return m.x >= pos.x && m.y >= pos.y && m.x < pos.x + size.x && m.y < pos.y + size.y;
}

Ui_Interact ui_interact(Ui *ui, u64 id, vec2 pos, vec2 size, Platform_Cursor cursor) {
    Ui_Interact r = {0};
    const Platform_Input *in = ui->in;
    b32 over = ui_mouse_in(ui, pos, size) && (ui->active == 0 || ui->active == id);

    if (over) {
        r.hovered = true;
        ui->hot = id;
        ui->cursor = cursor;
        if (in->mouse_pressed[PLATFORM_MOUSE_LEFT] && !ui->mouse_taken) {
            ui->active = id;
            ui->mouse_taken = true;
            r.pressed = true;
        }
        if (in->mouse_pressed[PLATFORM_MOUSE_RIGHT] && !ui->mouse_taken) {
            ui->mouse_taken = true;
            r.right_clicked = true;
        }
    }
    if (ui->active == id) {
        r.held = in->mouse_down[PLATFORM_MOUSE_LEFT];
        if (in->mouse_released[PLATFORM_MOUSE_LEFT]) {
            if (ui_mouse_in(ui, pos, size)) r.clicked = true;
            ui->active = 0;
        }
    }
    r.hover_t = ui_ease(ui, ui_idx(id, 0x401), r.hovered || r.held ? 1.0f : 0.0f, 18.0f);
    r.press_t = ui_ease(ui, ui_idx(id, 0x402), r.held && r.hovered ? 1.0f : 0.0f, 30.0f);
    return r;
}

void ui_block(Ui *ui, vec2 pos, vec2 size) {
    if (ui_mouse_in(ui, pos, size)) {
        if (ui->in->mouse_pressed[PLATFORM_MOUSE_LEFT] || ui->in->mouse_pressed[PLATFORM_MOUSE_RIGHT])
            ui->mouse_taken = true;
        if (!ui->hot) ui->hot = 1;
    }
}

/* ---- effects ---- */

void ui_ripple(Ui *ui, vec2 center, f32 radius, vec4 color, vec2 clip_pos, vec2 clip_size) {
    if (ui->ripple_count == UI_MAX_RIPPLES) {
        memmove(ui->ripples, ui->ripples + 1, sizeof(Ui_Ripple) * (UI_MAX_RIPPLES - 1));
        ui->ripple_count--;
    }
    ui->ripples[ui->ripple_count++] = (Ui_Ripple){
        .center = center, .radius = radius, .color = color,
        .clip_pos = clip_pos, .clip_size = clip_size,
    };
    ui->animating = true;
}

static u32 g_rng = 0x12345678u;
static f32 frand(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (f32)(g_rng & 0xFFFFFF) / (f32)0x1000000;
}

void ui_burst(Ui *ui, vec2 center, u32 count, vec4 color, u32 shape, f32 speed) {
    for (u32 i = 0; i < count && ui->particle_count < UI_MAX_PARTICLES; i++) {
        f32 ang = ((f32)i / (f32)count) * CORE_TAU + frand() * 0.6f;
        f32 sp = speed * (0.6f + frand() * 0.7f);
        Ui_Particle p = {
            .pos = center,
            .vel = vec2_make(cosf(ang) * sp, sinf(ang) * sp - speed * 0.25f),
            .life = 0.5f + frand() * 0.4f,
            .size = (shape == 1 ? 5.0f : 2.2f) * (0.7f + frand() * 0.6f) * ui->scale,
            .color = color,
            .shape = shape,
        };
        p.max_life = p.life;
        ui->particles[ui->particle_count++] = p;
    }
    ui->animating = true;
}

void ui_draw_effects(Ui *ui) {
    Core_Renderer *r = ui->r;
    for (u32 i = 0; i < ui->ripple_count; i++) {
        Ui_Ripple *rp = &ui->ripples[i];
        f32 t = rp->t;
        f32 e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        core_clip_push(r, rp->clip_pos, rp->clip_size);
        core_draw_circle_ex(r, rp->center, rp->radius * (0.15f + 0.85f * e), 0, S(2),
                            ui_alpha(rp->color, (1.0f - t) * 0.9f));
        core_clip_pop(r);
    }
    core_set_blend(r, CORE_BLEND_ADD);
    for (u32 i = 0; i < ui->particle_count; i++) {
        Ui_Particle *p = &ui->particles[i];
        f32 k = p->life / p->max_life;
        f32 sz = p->size * (0.4f + 0.6f * k);
        vec4 c = ui_alpha(p->color, k);
        if (p->shape == 1) ui_icon_heart(ui, p->pos, sz * 2.0f, c);
        else core_draw_circle_ex(r, p->pos, sz, 0, sz * 0.8f, c);
    }
    core_set_blend(r, CORE_BLEND_NORMAL);
}

/* ---- text ---- */

f32 ui_text_width(Ui *ui, Core_Font f, Core_String s, f32 px) {
    return core_text_measure(ui->text, f, s, px);
}

f32 ui_text(Ui *ui, Core_Font f, Core_String s, f32 x, f32 y, f32 px, vec4 color,
            Ui_Align align, f32 max_w) {
    f32 w = core_text_measure(ui->text, f, s, px);
    f32 shown = (max_w > 0 && w > max_w) ? max_w : w;
    if (align == UI_ALIGN_CENTER) x -= shown * 0.5f;
    else if (align == UI_ALIGN_RIGHT) x -= shown;
    Core_FontMetrics m = core_text_metrics(ui->text, f, px);
    f32 baseline = y + m.cap_height * 0.5f;
    if (max_w > 0) return core_text_draw_fit(ui->text, f, s, vec2_make(x, baseline), px, color, max_w);
    return core_text_draw(ui->text, f, s, vec2_make(x, baseline), px, color);
}

/* ---- icons ---- */

void ui_icon_play(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.5f;
    f32 rr = s * 0.08f;
    /* optically centered: shift right a little */
    f32 ox = s * 0.08f;
    core_draw_triangle_rounded(ui->r,
        vec2_make(c.x - h * 0.75f + ox + rr, c.y - h + rr * 1.6f),
        vec2_make(c.x + h * 0.95f + ox - rr * 1.8f, c.y),
        vec2_make(c.x - h * 0.75f + ox + rr, c.y + h - rr * 1.6f), rr, col);
}

void ui_icon_pause(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 bw = s * 0.26f, bh = s * 0.9f, gap = s * 0.2f;
    core_draw_rect_rounded(ui->r, vec2_make(c.x - gap * 0.5f - bw, c.y - bh * 0.5f), vec2_make(bw, bh), bw * 0.35f, col);
    core_draw_rect_rounded(ui->r, vec2_make(c.x + gap * 0.5f, c.y - bh * 0.5f), vec2_make(bw, bh), bw * 0.35f, col);
}

void ui_icon_prev(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.5f, rr = s * 0.06f;
    core_draw_rect_rounded(ui->r, vec2_make(c.x - h * 0.9f, c.y - h * 0.82f), vec2_make(s * 0.14f, s * 0.82f), s * 0.05f, col);
    core_draw_triangle_rounded(ui->r,
        vec2_make(c.x + h * 0.8f - rr, c.y - h * 0.82f + rr * 1.6f),
        vec2_make(c.x - h * 0.55f + rr * 1.8f, c.y),
        vec2_make(c.x + h * 0.8f - rr, c.y + h * 0.82f - rr * 1.6f), rr, col);
}

void ui_icon_next(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.5f, rr = s * 0.06f;
    core_draw_rect_rounded(ui->r, vec2_make(c.x + h * 0.9f - s * 0.14f, c.y - h * 0.82f), vec2_make(s * 0.14f, s * 0.82f), s * 0.05f, col);
    core_draw_triangle_rounded(ui->r,
        vec2_make(c.x - h * 0.8f + rr, c.y - h * 0.82f + rr * 1.6f),
        vec2_make(c.x + h * 0.55f - rr * 1.8f, c.y),
        vec2_make(c.x - h * 0.8f + rr, c.y + h * 0.82f - rr * 1.6f), rr, col);
}

static void arrow_head(Ui *ui, vec2 tip, vec2 dir, f32 len, f32 th, vec4 col) {
    vec2 d = vec2_normalize_safe(dir);
    vec2 n = vec2_perpendicular(d);
    vec2 back = vec2_sub(tip, vec2_mul(d, len));
    core_draw_line(ui->r, tip, vec2_add(back, vec2_mul(n, len * 0.85f)), th, col);
    core_draw_line(ui->r, tip, vec2_sub(back, vec2_mul(n, len * 0.85f)), th, col);
}

/* Cubic-ish S curve from a to b made of short capsules. */
static void s_curve(Ui *ui, vec2 a, vec2 b, f32 th, vec4 col) {
    const u32 N = 10;
    vec2 prev = a;
    for (u32 i = 1; i <= N; i++) {
        f32 t = (f32)i / (f32)N;
        f32 e = t * t * (3.0f - 2.0f * t);
        vec2 p = vec2_make(core_lerp(a.x, b.x, t), core_lerp(a.y, b.y, e));
        core_draw_line(ui->r, prev, p, th, col);
        prev = p;
    }
}

void ui_icon_shuffle(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.5f, th = s * 0.085f;
    f32 y0 = c.y - h * 0.55f, y1 = c.y + h * 0.55f;
    f32 x0 = c.x - h, x1 = c.x + h * 0.85f;
    /* two crossing strands */
    core_draw_line(ui->r, vec2_make(x0, y1), vec2_make(x0 + h * 0.35f, y1), th, col);
    s_curve(ui, vec2_make(x0 + h * 0.35f, y1), vec2_make(x1 - h * 0.45f, y0), th, col);
    core_draw_line(ui->r, vec2_make(x1 - h * 0.45f, y0), vec2_make(x1, y0), th, col);
    core_draw_line(ui->r, vec2_make(x0, y0), vec2_make(x0 + h * 0.35f, y0), th, col);
    s_curve(ui, vec2_make(x0 + h * 0.35f, y0), vec2_make(x1 - h * 0.45f, y1), th, col);
    core_draw_line(ui->r, vec2_make(x1 - h * 0.45f, y1), vec2_make(x1, y1), th, col);
    arrow_head(ui, vec2_make(x1 + th * 0.5f, y0), vec2_make(1, 0), s * 0.2f, th, col);
    arrow_head(ui, vec2_make(x1 + th * 0.5f, y1), vec2_make(1, 0), s * 0.2f, th, col);
}

void ui_icon_repeat(Ui *ui, vec2 c, f32 s, vec4 col, b32 one) {
    f32 h = s * 0.5f, th = s * 0.085f;
    f32 rw = h * 0.95f, rh = h * 0.55f, rad = rh;
    /* top run (left->right) and bottom run (right->left) joined by arcs */
    core_draw_line(ui->r, vec2_make(c.x - rw + rad, c.y - rh), vec2_make(c.x + rw - rad * 0.2f, c.y - rh), th, col);
    core_draw_line(ui->r, vec2_make(c.x + rw - rad, c.y + rh), vec2_make(c.x - rw + rad * 0.2f, c.y + rh), th, col);
    core_draw_arc(ui->r, vec2_make(c.x + rw - rad, c.y), rad, th, -CORE_PI * 0.5f + 0.9f, CORE_PI - 0.9f, col);
    core_draw_arc(ui->r, vec2_make(c.x - rw + rad, c.y), rad, th, CORE_PI * 0.5f + 0.9f, CORE_PI - 0.9f, col);
    arrow_head(ui, vec2_make(c.x + rw - rad * 0.2f + th * 0.3f, c.y - rh), vec2_make(1, 0), s * 0.18f, th, col);
    arrow_head(ui, vec2_make(c.x - rw + rad * 0.2f - th * 0.3f, c.y + rh), vec2_make(-1, 0), s * 0.18f, th, col);
    if (one) {
        core_draw_circle(ui->r, vec2_make(c.x + h * 0.95f, c.y - h * 0.8f), s * 0.2f, col);
        core_draw_line(ui->r, vec2_make(c.x + h * 0.95f, c.y - h * 0.8f - s * 0.09f),
                       vec2_make(c.x + h * 0.95f, c.y - h * 0.8f + s * 0.09f), s * 0.06f, UI_BG);
    }
}

void ui_icon_heart(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 r = s * 0.27f;
    f32 dx = s * 0.22f, cy = c.y - s * 0.14f;
    core_draw_circle(ui->r, vec2_make(c.x - dx, cy), r, col);
    core_draw_circle(ui->r, vec2_make(c.x + dx, cy), r, col);
    core_draw_triangle_rounded(ui->r,
        vec2_make(c.x - dx - r * 0.93f, cy + r * 0.28f),
        vec2_make(c.x + dx + r * 0.93f, cy + r * 0.28f),
        vec2_make(c.x, c.y + s * 0.42f), s * 0.04f, col);
}

void ui_icon_heart_outline(Ui *ui, vec2 c, f32 s, f32 stroke, vec4 col) {
    f32 r = s * 0.25f;
    f32 dx = s * 0.22f, cy = c.y - s * 0.14f;
    core_draw_arc(ui->r, vec2_make(c.x - dx, cy), r, stroke, CORE_PI * 0.75f, CORE_PI * 1.25f, col);
    core_draw_arc(ui->r, vec2_make(c.x + dx, cy), r, stroke, CORE_PI, CORE_PI * 1.25f, col);
    vec2 bottom = vec2_make(c.x, c.y + s * 0.4f);
    core_draw_line(ui->r, vec2_make(c.x - dx - r * 0.72f, cy + r * 0.7f), bottom, stroke, col);
    core_draw_line(ui->r, vec2_make(c.x + dx + r * 0.72f, cy + r * 0.7f), bottom, stroke, col);
}

void ui_icon_note(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.09f;
    vec2 h1 = vec2_make(c.x - s * 0.24f, c.y + s * 0.3f);
    vec2 h2 = vec2_make(c.x + s * 0.3f, c.y + s * 0.2f);
    f32 hr = s * 0.15f;
    core_draw_circle_ex(ui->r, h1, hr, th, 0, col);
    core_draw_circle_ex(ui->r, h2, hr, th, 0, col);
    vec2 t1 = vec2_make(h1.x + hr, c.y - s * 0.38f);
    vec2 t2 = vec2_make(h2.x + hr, c.y - s * 0.48f);
    core_draw_line(ui->r, vec2_make(h1.x + hr, h1.y), t1, th, col);
    core_draw_line(ui->r, vec2_make(h2.x + hr, h2.y), t2, th, col);
    core_draw_line(ui->r, t1, t2, th * 1.3f, col);
}

void ui_icon_person(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.085f;
    core_draw_circle_ex(ui->r, vec2_make(c.x, c.y - s * 0.2f), s * 0.2f, th, 0, col);
    core_draw_arc(ui->r, vec2_make(c.x, c.y + s * 0.5f), s * 0.36f, th, CORE_PI * 1.08f, CORE_PI * 0.84f, col);
}

void ui_icon_disc(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.085f;
    core_draw_circle_ex(ui->r, c, s * 0.44f, th, 0, col);
    core_draw_circle_ex(ui->r, c, s * 0.12f, th, 0, col);
}

void ui_icon_folder(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.085f, x0 = c.x - s * 0.44f, y0 = c.y - s * 0.2f;
    Core_BoxStyle st = { .radius = s * 0.1f, .fill = ui_alpha(col, 0), .border = th, .border_color = col };
    core_draw_box(ui->r, vec2_make(x0, y0), vec2_make(s * 0.88f, s * 0.6f), &st);
    /* tab on the top-left edge */
    vec2 a = vec2_make(x0 + s * 0.06f, y0), b = vec2_make(x0 + s * 0.1f, y0 - s * 0.14f);
    vec2 d = vec2_make(x0 + s * 0.36f, y0 - s * 0.14f), e = vec2_make(x0 + s * 0.44f, y0);
    core_draw_line(ui->r, a, b, th, col);
    core_draw_line(ui->r, b, d, th, col);
    core_draw_line(ui->r, d, e, th, col);
}

void ui_icon_gear(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.085f;
    for (u32 i = 0; i < 8; i++) {
        f32 a = (f32)i / 8.0f * CORE_TAU;
        vec2 d = vec2_make(cosf(a), sinf(a));
        /* stubby teeth fused to the ring */
        core_draw_line(ui->r, vec2_add(c, vec2_mul(d, s * 0.33f)), vec2_add(c, vec2_mul(d, s * 0.41f)), s * 0.15f, col);
    }
    core_draw_circle_ex(ui->r, c, s * 0.31f, th * 1.1f, 0, col);
    core_draw_circle_ex(ui->r, c, s * 0.12f, th, 0, col);
}

void ui_icon_kebab(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 r = s * 0.1f;
    for (s32 i = -1; i <= 1; i++)
        core_draw_circle(ui->r, vec2_make(c.x, c.y + (f32)i * s * 0.32f), r, col);
}

void ui_icon_minimize(Ui *ui, vec2 c, f32 s, vec4 col) {
    core_draw_line(ui->r, vec2_make(c.x - s * 0.45f, c.y), vec2_make(c.x + s * 0.45f, c.y), s * 0.1f, col);
}

void ui_icon_maximize(Ui *ui, vec2 c, f32 s, vec4 col) {
    Core_BoxStyle st = { .radius = s * 0.12f, .fill = ui_alpha(col, 0), .border = s * 0.1f, .border_color = col };
    core_draw_box(ui->r, vec2_make(c.x - s * 0.42f, c.y - s * 0.42f), vec2_make(s * 0.84f, s * 0.84f), &st);
}

void ui_icon_close(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.42f, th = s * 0.1f;
    core_draw_line(ui->r, vec2_make(c.x - h, c.y - h), vec2_make(c.x + h, c.y + h), th, col);
    core_draw_line(ui->r, vec2_make(c.x - h, c.y + h), vec2_make(c.x + h, c.y - h), th, col);
}

void ui_icon_chevron_right(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.3f, th = s * 0.1f;
    core_draw_line(ui->r, vec2_make(c.x - h * 0.5f, c.y - h), vec2_make(c.x + h * 0.5f, c.y), th, col);
    core_draw_line(ui->r, vec2_make(c.x + h * 0.5f, c.y), vec2_make(c.x - h * 0.5f, c.y + h), th, col);
}

void ui_icon_chevron_left(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.3f, th = s * 0.1f;
    core_draw_line(ui->r, vec2_make(c.x + h * 0.5f, c.y - h), vec2_make(c.x - h * 0.5f, c.y), th, col);
    core_draw_line(ui->r, vec2_make(c.x - h * 0.5f, c.y), vec2_make(c.x + h * 0.5f, c.y + h), th, col);
}

void ui_icon_enter(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.09f;
    vec2 top = vec2_make(c.x + s * 0.32f, c.y - s * 0.3f);
    vec2 corner = vec2_make(c.x + s * 0.32f, c.y + s * 0.12f);
    vec2 tip = vec2_make(c.x - s * 0.34f, c.y + s * 0.12f);
    core_draw_line(ui->r, top, corner, th, col);
    core_draw_line(ui->r, corner, tip, th, col);
    arrow_head(ui, tip, vec2_make(-1, 0), s * 0.2f, th, col);
}

void ui_icon_search(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.1f;
    vec2 o = vec2_make(c.x - s * 0.08f, c.y - s * 0.08f);
    core_draw_circle_ex(ui->r, o, s * 0.28f, th, 0, col);
    core_draw_line(ui->r, vec2_make(o.x + s * 0.21f, o.y + s * 0.21f), vec2_make(c.x + s * 0.4f, c.y + s * 0.4f), th * 1.2f, col);
}

void ui_icon_volume(Ui *ui, vec2 c, f32 s, f32 level, vec4 col) {
    f32 th = s * 0.09f;
    core_draw_rect_rounded(ui->r, vec2_make(c.x - s * 0.45f, c.y - s * 0.14f), vec2_make(s * 0.18f, s * 0.28f), s * 0.03f, col);
    core_draw_triangle_rounded(ui->r, vec2_make(c.x - s * 0.3f, c.y - s * 0.14f),
                               vec2_make(c.x - s * 0.05f, c.y - s * 0.38f),
                               vec2_make(c.x - s * 0.05f, c.y + s * 0.38f), s * 0.02f, col);
    core_draw_triangle_rounded(ui->r, vec2_make(c.x - s * 0.3f, c.y - s * 0.14f),
                               vec2_make(c.x - s * 0.05f, c.y + s * 0.38f),
                               vec2_make(c.x - s * 0.3f, c.y + s * 0.14f), s * 0.02f, col);
    if (level > 0.01f)
        core_draw_arc(ui->r, vec2_make(c.x, c.y), s * 0.2f, th, -0.8f, 1.6f, col);
    if (level > 0.5f)
        core_draw_arc(ui->r, vec2_make(c.x, c.y), s * 0.38f, th, -0.8f, 1.6f, col);
}

void ui_icon_plus(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.38f, th = s * 0.1f;
    core_draw_line(ui->r, vec2_make(c.x - h, c.y), vec2_make(c.x + h, c.y), th, col);
    core_draw_line(ui->r, vec2_make(c.x, c.y - h), vec2_make(c.x, c.y + h), th, col);
}

void ui_icon_minus(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.38f, th = s * 0.1f;
    core_draw_line(ui->r, vec2_make(c.x - h, c.y), vec2_make(c.x + h, c.y), th, col);
}

void ui_icon_download(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.095f;
    vec2 top = vec2_make(c.x, c.y - s * 0.38f), tip = vec2_make(c.x, c.y + s * 0.12f);
    core_draw_line(ui->r, top, tip, th, col);
    arrow_head(ui, vec2_make(tip.x, tip.y + th * 0.4f), vec2_make(0, 1), s * 0.22f, th, col);
    /* tray */
    f32 x0 = c.x - s * 0.38f, x1 = c.x + s * 0.38f, y0 = c.y + s * 0.16f, y1 = c.y + s * 0.4f;
    core_draw_line(ui->r, vec2_make(x0, y0), vec2_make(x0, y1), th, col);
    core_draw_line(ui->r, vec2_make(x0, y1), vec2_make(x1, y1), th, col);
    core_draw_line(ui->r, vec2_make(x1, y1), vec2_make(x1, y0), th, col);
}

void ui_icon_check(Ui *ui, vec2 c, f32 s, f32 t, vec4 col) {
    f32 th = s * 0.11f;
    vec2 a = vec2_make(c.x - s * 0.32f, c.y + s * 0.02f);
    vec2 b = vec2_make(c.x - s * 0.1f, c.y + s * 0.24f);
    vec2 d = vec2_make(c.x + s * 0.34f, c.y - s * 0.24f);
    if (t <= 0) return;
    f32 l1 = vec2_distance(a, b), l2 = vec2_distance(b, d);
    f32 at = t * (l1 + l2);
    if (at <= l1) {
        core_draw_line(ui->r, a, vec2_lerp(a, b, at / l1), th, col);
    } else {
        core_draw_line(ui->r, a, b, th, col);
        core_draw_line(ui->r, b, vec2_lerp(b, d, CORE_MIN(1.0f, (at - l1) / l2)), th, col);
    }
}

void ui_icon_retry(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.095f, r = s * 0.32f;
    f32 a0 = -CORE_PI * 0.1f, sweep = CORE_PI * 1.5f, end = a0 + sweep;
    core_draw_arc(ui->r, c, r, th, a0, sweep, col);
    vec2 tip = vec2_make(c.x + cosf(end) * r, c.y + sinf(end) * r);
    arrow_head(ui, tip, vec2_make(-sinf(end), cosf(end)), s * 0.22f, th, col);
}

void ui_icon_chevron_down(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.3f, th = s * 0.1f;
    core_draw_line(ui->r, vec2_make(c.x - h, c.y - h * 0.5f), vec2_make(c.x, c.y + h * 0.5f), th, col);
    core_draw_line(ui->r, vec2_make(c.x, c.y + h * 0.5f), vec2_make(c.x + h, c.y - h * 0.5f), th, col);
}

void ui_icon_chevron_up(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 h = s * 0.3f, th = s * 0.1f;
    core_draw_line(ui->r, vec2_make(c.x - h, c.y + h * 0.5f), vec2_make(c.x, c.y - h * 0.5f), th, col);
    core_draw_line(ui->r, vec2_make(c.x, c.y - h * 0.5f), vec2_make(c.x + h, c.y + h * 0.5f), th, col);
}

void ui_icon_warning(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.09f;
    core_draw_circle_ex(ui->r, c, s * 0.42f, th, 0, col);
    core_draw_line(ui->r, vec2_make(c.x, c.y - s * 0.2f), vec2_make(c.x, c.y + s * 0.04f), th, col);
    core_draw_circle(ui->r, vec2_make(c.x, c.y + s * 0.22f), th * 0.65f, col);
}

void ui_icon_queue_next(Ui *ui, vec2 c, f32 s, vec4 col) {
    f32 th = s * 0.09f, h = s * 0.42f;
    for (u32 i = 0; i < 3; i++) {
        f32 y = c.y - h * 0.6f + (f32)i * h * 0.6f;
        core_draw_line(ui->r, vec2_make(c.x - h, y), vec2_make(c.x + h * (i == 2 ? 0.1f : 0.5f), y), th, col);
    }
    core_draw_triangle_rounded(ui->r, vec2_make(c.x + h * 0.45f, c.y + h * 0.3f),
                               vec2_make(c.x + h, c.y + h * 0.6f),
                               vec2_make(c.x + h * 0.45f, c.y + h * 0.9f), s * 0.02f, col);
}

void ui_icon_eq(Ui *ui, vec2 c, f32 s, const f32 *levels, vec4 col) {
    f32 bw = s * 0.16f, gap = s * 0.1f;
    f32 total = bw * 4 + gap * 3;
    f32 x = c.x - total * 0.5f;
    f32 bottom = c.y + s * 0.42f;
    for (u32 i = 0; i < 4; i++) {
        f32 h = s * (0.2f + 0.64f * CORE_CLAMP(levels[i], 0.0f, 1.0f));
        core_draw_rect_rounded(ui->r, vec2_make(x, bottom - h), vec2_make(bw, h), bw * 0.3f, col);
        x += bw + gap;
    }
}

f32 ui_keycap(Ui *ui, Core_String label, f32 x, f32 cy, f32 h, Ui_Align align, f32 alpha) {
    f32 px = h * 0.56f;
    f32 tw = ui_text_width(ui, ui->font_mono, label, px);
    f32 w = CORE_MAX(tw + h * 0.62f, h * 1.05f);
    if (align == UI_ALIGN_RIGHT) x -= w;
    else if (align == UI_ALIGN_CENTER) x -= w * 0.5f;
    Core_BoxStyle st = {
        .radius = S(5), .fill = UI_RGBA(255, 255, 255, 0.04f * alpha),
        .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.16f * alpha),
    };
    core_draw_box(ui->r, vec2_make(x, cy - h * 0.5f), vec2_make(w, h), &st);
    ui_text(ui, ui->font_mono, label, x + w * 0.5f, cy, px, UI_RGBA(210, 206, 228, alpha), UI_ALIGN_CENTER, 0);
    return w;
}
