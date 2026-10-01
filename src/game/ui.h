#ifndef GAME_UI_H
#define GAME_UI_H

#include "../core/font.h"
#include "../core/renderer.h"
#include "../platform/platform.h"

/*
 * Immediate-mode UI helpers for Offbeat.
 *
 * Widgets are functions called every frame with a stable id; interaction is
 * resolved against this frame's input. Every visual transition goes through
 * the animation table (keyed by id), so hover/press/selection states glide
 * instead of snapping, and the app knows exactly when something is still
 * moving (ui->animating) -- when nothing is, the main loop sleeps.
 *
 * All sizes are "design pixels" multiplied by ui->scale (the window-size
 * dependent zoom), via the S() helper.
 */

typedef struct {
    u64 id;
    f32 value;
    f32 velocity;
    u32 touched;   /* frame index of last use (for eviction) */
} Ui_Anim;

typedef struct {
    vec2 pos;
    vec2 vel;
    f32  life;     /* seconds remaining */
    f32  max_life;
    f32  size;
    vec4 color;
    u32  shape;    /* 0 circle, 1 heart */
} Ui_Particle;

typedef struct {
    vec2 center;
    f32  t;        /* 0..1 progress */
    f32  radius;
    vec4 color;
    vec2 clip_pos, clip_size;
} Ui_Ripple;

#define UI_MAX_PARTICLES 256
#define UI_MAX_RIPPLES   16

typedef struct {
    Core_Renderer *r;
    Core_Text     *text;
    Core_Font      font;       /* Outfit Regular + fallbacks  */
    Core_Font      font_med;   /* Outfit Medium               */
    Core_Font      font_semi;  /* Outfit SemiBold             */
    Core_Font      font_mono;  /* JetBrains Mono              */

    const Platform_Input *in;
    f32  dt;
    f64  time;
    u32  frame;
    f32  scale;
    vec2 size;                 /* window size */

    /* interaction */
    b32  input_enabled;        /* false while a modal covers this layer */
    u64  hot;                  /* hovered this frame                    */
    u64  active;               /* pressed and held                      */
    b32  mouse_taken;          /* a widget claimed the press this frame */
    Platform_Cursor cursor;

    /* double-click detection */
    f64  last_click_time;
    vec2 last_click_pos;
    b32  double_click;         /* this frame's press is a double click */

    Ui_Anim *anims;
    u32      anim_cap;

    Ui_Particle particles[UI_MAX_PARTICLES];
    u32         particle_count;
    Ui_Ripple   ripples[UI_MAX_RIPPLES];
    u32         ripple_count;

    b32 animating;             /* something is still in motion */
} Ui;

/* ---- theme ---- */

#define UI_RGBA_C(r, g, b, a) { .e = { (r) / 255.0f, (g) / 255.0f, (b) / 255.0f, (a) } }
#define UI_RGBA(r, g, b, a) ((vec4)UI_RGBA_C(r, g, b, a))

extern const vec4 UI_BG;
extern const vec4 UI_PANEL;
extern const vec4 UI_TEXT;
extern const vec4 UI_TEXT_DIM;
extern const vec4 UI_TEXT_FAINT;
extern const vec4 UI_ACCENT;
extern const vec4 UI_ACCENT_BRIGHT;
extern const vec4 UI_LAVENDER;
extern const vec4 UI_LINE;

vec4 ui_alpha(vec4 c, f32 a);                 /* multiply alpha        */
vec4 ui_mix(vec4 a, vec4 b, f32 t);
vec4 ui_rgb(vec3 c, f32 a);
/* Deterministic violet placeholder color for artwork missing from either view. */
vec4 game_ui_hash_color(u64 hash, f32 alpha);

/* ---- frame ---- */

void ui_init(Ui *ui, Core_Arena *arena, Core_Renderer *r, Core_Text *text);
void ui_begin(Ui *ui, const Platform_Input *in, f32 dt, vec2 size, f32 scale);
void ui_end(Ui *ui);   /* updates particles/ripples, resolves cursor */

#define S(v) ((v) * ui->scale)

/* ---- ids ---- */

u64 ui_id(const char *name);
u64 ui_idx(u64 base, u64 index);

/* ---- animation ----
   ui_ease: exponential approach at `rate` (1/s). ui_spring: damped spring
   (bouncy when damping is low). Both return the current value and keep
   ui->animating set until settled. `ui_anim_set` snaps. */
f32  ui_ease(Ui *ui, u64 id, f32 target, f32 rate);
f32  ui_spring(Ui *ui, u64 id, f32 target, f32 stiffness, f32 damping);
void ui_anim_set(Ui *ui, u64 id, f32 value);
f32  ui_anim_get(Ui *ui, u64 id, f32 fallback);
/* Kick a spring's velocity (impulse feedback, e.g. press pop). */
void ui_anim_kick(Ui *ui, u64 id, f32 velocity);

/* ---- interaction ---- */

typedef struct {
    b32 hovered;
    b32 pressed;   /* mouse went down on it this frame  */
    b32 held;      /* is the active widget              */
    b32 clicked;   /* released over it after pressing   */
    b32 right_clicked;
    f32 hover_t;   /* animated 0..1                     */
    f32 press_t;   /* animated 0..1                     */
} Ui_Interact;

b32         ui_mouse_in(Ui *ui, vec2 pos, vec2 size);
Ui_Interact ui_interact(Ui *ui, u64 id, vec2 pos, vec2 size, Platform_Cursor cursor);
/* Mark a region as solid so clicks don't fall through to layers below. */
void        ui_block(Ui *ui, vec2 pos, vec2 size);

/* ---- effects ---- */

void ui_ripple(Ui *ui, vec2 center, f32 radius, vec4 color, vec2 clip_pos, vec2 clip_size);
void ui_burst(Ui *ui, vec2 center, u32 count, vec4 color, u32 shape, f32 speed);
void ui_draw_effects(Ui *ui);   /* ripples + particles, call last */

/* ---- text ---- */

typedef enum { UI_ALIGN_LEFT, UI_ALIGN_CENTER, UI_ALIGN_RIGHT } Ui_Align;

/* Draw text vertically centered on `y` (cap-height centering), aligned on x.
   `max_w` > 0 truncates with an ellipsis. Returns width. */
f32 ui_text(Ui *ui, Core_Font f, Core_String s, f32 x, f32 y, f32 px, vec4 color,
            Ui_Align align, f32 max_w);
f32 ui_text_width(Ui *ui, Core_Font f, Core_String s, f32 px);

/* ---- icons (drawn with SDF primitives; `c` center, `s` nominal size px) ---- */

void ui_icon_play(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_pause(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_prev(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_next(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_shuffle(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_repeat(Ui *ui, vec2 c, f32 s, vec4 col, b32 one);
void ui_icon_heart(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_heart_outline(Ui *ui, vec2 c, f32 s, f32 stroke, vec4 col);
void ui_icon_note(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_person(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_disc(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_folder(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_gear(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_kebab(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_minimize(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_maximize(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_close(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_chevron_right(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_chevron_left(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_enter(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_search(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_volume(Ui *ui, vec2 c, f32 s, f32 level, vec4 col);
void ui_icon_plus(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_queue_next(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_minus(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_download(Ui *ui, vec2 c, f32 s, vec4 col);
/* Check mark drawn up to `t` (0..1) so it can animate in. */
void ui_icon_check(Ui *ui, vec2 c, f32 s, f32 t, vec4 col);
void ui_icon_retry(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_chevron_up(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_chevron_down(Ui *ui, vec2 c, f32 s, vec4 col);
void ui_icon_warning(Ui *ui, vec2 c, f32 s, vec4 col);
/* Animated equalizer bars (now-playing marker); `levels` 0..1 x4. */
void ui_icon_eq(Ui *ui, vec2 c, f32 s, const f32 *levels, vec4 col);

/* Keycap (e.g. "Ctrl", "K", "Enter"): rounded box with mono label, faded by `alpha`. Returns width. */
f32 ui_keycap(Ui *ui, Core_String label, f32 x, f32 cy, f32 h, Ui_Align align, f32 alpha);

#endif /* GAME_UI_H */
