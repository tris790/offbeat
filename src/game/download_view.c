/*
 * The "Get music" page. See download_view.h.
 *
 * Layout is one centered column (max ~880 design px) under a header: search
 * bar, then the body (results / messages), with the download dock floating
 * over the bottom. The search bar sits in the middle of the page until the
 * first results are in, then glides up under the header and stays there. Everything is immediate-mode and scaled by
 * ui->scale like the rest of the app; all motion goes through the Ui
 * animation table so hover, press and appear effects glide.
 *
 * The dock is drawn last (it floats over the list) but its geometry is needed
 * first, to keep list rows from reacting to clicks meant for the dock: the
 * page remembers last frame's dock rectangle for that.
 */

#include "download_view.h"

#include "ytthumbs.h"

#include "../platform/platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_JOBS_SHOWN 1024

typedef struct {
    f32 target, pos, idle, bar_t;
} Scroll;

typedef struct { u64 key; u32 track; } Lib_Key;

typedef enum { ROW_FREE = 0, ROW_LIB, ROW_QUEUED, ROW_ACTIVE, ROW_DONE, ROW_FAILED } Row_State;

struct Dl_View {
    Downloads *d;
    Ythumbs   *thumbs;
    b32  open;

    /* search bar */
    u32  mode;                      /* Dl_QueryKind */
    char query[256];
    u32  query_len;
    f64  opened_at;
    f32  caret_hold;

    /* what the results belong to */
    char searched[256];
    u32  searched_kind;
    b32  has_searched;
    b32  bar_docked;                /* the search bar has moved to the top */
    u32  gen;
    b32  select_all_pending;
    u8   checked[DL_MAX_RESULTS];
    u8   libstate[DL_MAX_RESULTS];  /* 0 unknown, 1 not in the library, 2 in it */
    u64  libkey[DL_MAX_RESULTS];    /* what libstate was computed for (a result changes when its artist arrives) */
    f64  seen_at[DL_MAX_RESULTS];   /* when a result first appeared (stagger) */
    u32  seen_count;
    s32  sel;                       /* highlighted result */
    Scroll scroll;

    /* dock */
    f32  dock_t;                    /* 0 collapsed .. 1 expanded */
    b32  dock_pinned;
    f64  dock_flash_until;
    vec2 dock_pos, dock_size;       /* last frame's full rectangle */
    Scroll dock_scroll;
    Dl_Brief briefs[MAX_JOBS_SHOWN];
    u32  order[MAX_JOBS_SHOWN];

    f32  open_t;                    /* page visibility, for dlv_visible */
    f32  drift;                     /* backdrop animation clock: only runs while something is going on */
    Lib_Key *lib_keys;              /* library titles by song key, for "already in your library" */
    u32  lib_key_count;
    const Library *lib_keys_for;

    /* status pill on the player screen */
    b32  pill_was_busy;
    f64  pill_done_until;
    u32  pill_done_count;
    u32  pill_failed;
    f32  pill_expand;
};

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

static f32 clamp01(f32 x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static f32 ease_out_cubic(f32 t) { t = clamp01(t); f32 u = 1 - t; return 1 - u * u * u; }

static void copy_str(char *dst, u32 cap, const char *src) {
    snprintf(dst, cap, "%s", src);
}

static vec4 hash_color(u64 h, f32 a) {
    f32 hue = (f32)(h % 360) / 360.0f;
    f32 r = 0.5f + 0.5f * cosf(CORE_TAU * (hue + 0.0f));
    f32 g = 0.5f + 0.5f * cosf(CORE_TAU * (hue + 0.33f));
    f32 b = 0.5f + 0.5f * cosf(CORE_TAU * (hue + 0.67f));
    return (vec4){ .x = 0.18f + r * 0.28f, .y = 0.12f + g * 0.2f, .z = 0.3f + b * 0.3f, .w = a };
}

static u64 str_hash(const char *s) {
    u64 h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (u8)*s; h *= 1099511628211ull; }
    return h;
}

static void fmt_duration(char *out, u32 cap, u32 s) {
    if (s >= 3600) snprintf(out, cap, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, cap, "%u:%02u", s / 60, s % 60);
}

static void fmt_views(char *out, u32 cap, u64 v) {
    f64 n = (f64)v;
    const char *unit = "";
    if (v >= 1000000000ull) { n /= 1e9; unit = "B"; }
    else if (v >= 1000000ull) { n /= 1e6; unit = "M"; }
    else if (v >= 10000ull) { n /= 1e3; unit = "K"; }
    if (*unit && n < 100) { /* one decimal, unless it would be ".0" */
        u32 tenths = (u32)(n * 10.0 + 0.5);
        if (tenths % 10) snprintf(out, cap, "%u.%u%s views", tenths / 10, tenths % 10, unit);
        else snprintf(out, cap, "%u%s views", tenths / 10, unit);
    } else {
        snprintf(out, cap, "%u%s views", (u32)(n + 0.5), unit);
    }
}

static u64 key_hash(const char *key) {
    u64 h = 1469598103934665603ull;
    for (; *key; key++) { h ^= (u8)*key; h *= 1099511628211ull; }
    return h;
}

static int lib_key_cmp(const void *a, const void *b) {
    const Lib_Key *x = a, *y = b;
    return x->key < y->key ? -1 : x->key > y->key ? 1 : 0;
}

/* Library titles sorted by song key (release tags ignored), rebuilt whenever
   the library snapshot changes. */
static void sync_lib_keys(Dl_View *v, const Library *lib) {
    if (v->lib_keys_for == lib) return;
    core_heap_free(v->lib_keys);
    v->lib_keys = 0;
    v->lib_key_count = 0;
    v->lib_keys_for = lib;
    memset(v->libstate, 0, sizeof(v->libstate));
    if (!lib || !lib->track_count) return;
    v->lib_keys = core_heap_alloc(sizeof(Lib_Key) * lib->track_count);
    for (u32 i = 0; i < lib->track_count; i++) {
        char k[DL_TEXT * 2];
        dl_song_key(lib->tracks[i].title.str, k, sizeof(k));
        if (k[0]) v->lib_keys[v->lib_key_count++] = (Lib_Key){ .key = key_hash(k), .track = i };
    }
    qsort(v->lib_keys, v->lib_key_count, sizeof(Lib_Key), lib_key_cmp);
}

static b32 key_in_library(const Dl_View *v, const Library *lib, const char *song_key, const char *known, const char *raw_title) {
    if (!song_key[0]) return false;
    u64 h = key_hash(song_key);
    u32 lo = 0, hi = v->lib_key_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (v->lib_keys[mid].key < h) lo = mid + 1; else hi = mid;
    }
    for (u32 i = lo; i < v->lib_key_count && v->lib_keys[i].key == h; i++)
        if (dl_artist_plausible(lib->tracks[v->lib_keys[i].track].artist.str, known, raw_title)) return true;
    return false;
}

/* Does the library already have this result? Same title (modulo "(2011
   Remaster)" and friends) by a plausible artist. "Artist - Song" titles are
   tried both whole and split. */
static b32 in_library(const Dl_View *v, const Library *lib, const char *known, const Dl_Result *r) {
    if (!lib || !v->lib_key_count) return false;
    char k[DL_TEXT * 2];
    dl_song_key(r->title, k, sizeof(k));
    if (key_in_library(v, lib, k, known, r->title)) return true;
    const char *dash = strstr(r->title, " - ");
    if (!dash) return false;
    dl_song_key(dash + 3, k, sizeof(k)); /* "Artist - Song" */
    if (key_in_library(v, lib, k, known, r->title)) return true;
    char left[DL_TEXT];
    snprintf(left, sizeof(left), "%.*s", (int)CORE_MIN((size_t)(dash - r->title), sizeof(left) - 1), r->title);
    dl_song_key(left, k, sizeof(k)); /* "Song - Artist" */
    return key_in_library(v, lib, k, known, r->title);
}

/* ------------------------------------------------------------------------- */
/* widgets                                                                   */
/* ------------------------------------------------------------------------- */

static void glass(Ui *ui, vec2 p, vec2 sz, f32 radius, f32 a) {
    Core_Renderer *r = ui->r;
    core_draw_shadow(r, vec2_make(p.x, p.y + S(14)), sz, radius, S(40), UI_RGBA(0, 0, 0, 0.5f * a));
    core_draw_shadow(r, p, sz, radius, S(22), UI_RGBA(110, 70, 255, 0.10f * a));
    Core_BoxStyle bg = { .radius = radius, .fill = UI_RGBA(29, 25, 47, a), .fill2 = UI_RGBA(21, 18, 36, a),
                         .gradient = 1 };
    core_draw_box(r, p, sz, &bg);
    Core_BoxStyle edge = { .radius = radius, .fill = UI_RGBA(0, 0, 0, 0), .border = S(1),
                           .border_color = UI_RGBA(255, 255, 255, 0.11f * a) };
    core_draw_box(r, p, sz, &edge);
}

/* Round icon button with a hover disc. */
static Ui_Interact round_button(Ui *ui, u64 id, vec2 c, f32 radius, f32 a) {
    Ui_Interact it = ui_interact(ui, id, vec2_make(c.x - radius, c.y - radius), vec2_make(radius * 2, radius * 2),
                                 PLATFORM_CURSOR_HAND);
    if (it.hover_t > 0.01f)
        core_draw_circle(ui->r, c, radius * (0.92f + 0.08f * it.hover_t), UI_RGBA(255, 255, 255, 0.09f * it.hover_t * a));
    return it;
}

/* Pill button; `primary` is the filled accent one. Disabled ones ignore input. */
static Ui_Interact pill_button(Ui *ui, u64 id, Core_String label, vec2 pos, vec2 size, b32 primary, b32 enabled,
                               f32 a, void (*icon)(Ui *, vec2, f32, vec4)) {
    Core_Renderer *r = ui->r;
    b32 saved = ui->input_enabled;
    if (!enabled) ui->input_enabled = false;
    Ui_Interact it = ui_interact(ui, id, pos, size, PLATFORM_CURSOR_HAND);
    ui->input_enabled = saved;
    f32 en = ui_ease(ui, ui_idx(id, 7), enabled ? 1.0f : 0.0f, 12.0f);
    f32 rad = size.y * 0.5f;
    f32 lift = it.hover_t * en;
    if (primary) {
        if (en > 0.05f)
            core_draw_shadow(r, vec2_make(pos.x, pos.y + S(3)), size, rad, S(14), UI_RGBA(110, 70, 255, (0.25f + 0.2f * lift) * en * a));
        Core_BoxStyle st = { .radius = rad,
                             .fill = ui_alpha(ui_mix(UI_RGBA(118, 90, 226, 1), UI_RGBA(146, 116, 250, 1), lift), a),
                             .fill2 = UI_RGBA(100, 72, 210, a), .gradient = 1 };
        if (en < 1) { st.fill = ui_mix(UI_RGBA(255, 255, 255, 0.08f * a), st.fill, en); st.fill2 = ui_mix(UI_RGBA(255, 255, 255, 0.06f * a), st.fill2, en); }
        core_draw_box(r, pos, size, &st);
    } else {
        Core_BoxStyle st = { .radius = rad, .fill = UI_RGBA(255, 255, 255, (0.05f + 0.06f * lift) * a),
                             .border = S(1), .border_color = UI_RGBA(255, 255, 255, (0.12f + 0.08f * lift) * a) };
        core_draw_box(r, pos, size, &st);
    }
    if (it.pressed && enabled) ui_ripple(ui, ui->in->mouse_pos, size.x, UI_RGBA(190, 160, 255, 0.3f), pos, size);
    f32 px = S(14) * (1 - 0.03f * it.press_t);
    vec4 tc = ui_alpha(primary ? UI_TEXT : ui_mix(UI_RGBA(215, 210, 235, 1), UI_TEXT, lift), a * (0.45f + 0.55f * en));
    f32 lw = ui_text_width(ui, ui->font_med, label, px);
    f32 iw = icon ? S(20) : 0;
    f32 x = pos.x + (size.x - lw - iw) * 0.5f;
    if (icon) icon(ui, vec2_make(x + S(8), pos.y + size.y * 0.5f), S(17), tc);
    ui_text(ui, ui->font_med, label, x + iw, pos.y + size.y * 0.5f, px, tc, UI_ALIGN_LEFT, 0);
    return it;
}

static void checkbox(Ui *ui, u64 id, vec2 c, f32 size, b32 checked, b32 partial, f32 hover, f32 a) {
    Core_Renderer *r = ui->r;
    f32 t = ui_spring(ui, ui_idx(id, 1), (checked || partial) ? 1.0f : 0.0f, 460, 24);
    f32 tc = clamp01(t);
    f32 s = size * (1 + 0.14f * (t - tc) + 0.05f * hover);
    vec2 p = vec2_make(c.x - s * 0.5f, c.y - s * 0.5f);
    if (tc > 0.02f)
        core_draw_shadow(r, p, vec2_make(s, s), S(6), S(10), UI_RGBA(139, 92, 246, 0.35f * tc * a));
    Core_BoxStyle st = { .radius = S(6),
                         .fill = ui_alpha(ui_mix(UI_RGBA(255, 255, 255, 0.05f + 0.05f * hover), UI_RGBA(128, 98, 236, 1), tc), a),
                         .border = S(1.5f),
                         .border_color = ui_alpha(ui_mix(UI_RGBA(255, 255, 255, 0.26f + 0.22f * hover), UI_RGBA(171, 140, 255, 1), tc), a) };
    core_draw_box(r, p, vec2_make(s, s), &st);
    if (partial) ui_icon_minus(ui, c, size * 0.78f, ui_alpha(UI_TEXT, a * tc));
    else if (tc > 0.04f) ui_icon_check(ui, c, size * 0.82f, tc, ui_alpha(UI_TEXT, a));
}

static void spinner(Ui *ui, vec2 c, f32 radius, f32 th, vec4 col) {
    core_draw_circle_ex(ui->r, c, radius, th, 0, ui_alpha(col, 0.14f * col.w));
    f32 a0 = (f32)ui->time * 5.2f;
    f32 sweep = 1.3f + 0.6f * sinf((f32)ui->time * 3.1f);
    core_draw_arc(ui->r, c, radius, th, a0, sweep, col);
    ui->animating = true;
}

static void ring(Ui *ui, vec2 c, f32 radius, f32 th, f32 t, vec4 col, f32 a) {
    core_draw_circle_ex(ui->r, c, radius, th, 0, UI_RGBA(255, 255, 255, 0.12f * a));
    if (t > 0.004f) core_draw_arc(ui->r, c, radius, th, -CORE_PI * 0.5f, CORE_TAU * CORE_MIN(t, 0.999f), col);
}

/* Thin progress bar: soft glow under the fill, a slow light sweep over it. */
static void bar(Ui *ui, vec2 p, vec2 sz, f32 t, f32 a, b32 shimmer) {
    Core_Renderer *r = ui->r;
    core_draw_rect_rounded(r, p, sz, sz.y * 0.5f, UI_RGBA(255, 255, 255, 0.09f * a));
    f32 w = CORE_MAX(sz.y, sz.x * clamp01(t));
    if (t <= 0.001f) return;
    core_set_blend(r, CORE_BLEND_ADD);
    core_draw_shadow(r, p, vec2_make(w, sz.y), sz.y * 0.5f, S(8), UI_RGBA(139, 92, 246, 0.35f * a));
    core_set_blend(r, CORE_BLEND_NORMAL);
    Core_BoxStyle fill = { .radius = sz.y * 0.5f, .fill = UI_RGBA(139, 102, 255, a), .fill2 = UI_RGBA(196, 160, 255, a), .gradient = 2 };
    core_draw_box(r, p, vec2_make(w, sz.y), &fill);
    if (shimmer) {
        f32 ph = fmodf((f32)ui->time * 0.8f, 1.6f) - 0.3f;
        f32 sx = p.x + (w + S(30)) * ph - S(30);
        core_clip_push(r, p, vec2_make(w, sz.y));
        core_draw_rect_gradient(r, vec2_make(sx, p.y), vec2_make(S(30), sz.y), UI_RGBA(255, 255, 255, 0), UI_RGBA(255, 255, 255, 0.45f * a), true);
        core_draw_rect_gradient(r, vec2_make(sx + S(30), p.y), vec2_make(S(14), sz.y), UI_RGBA(255, 255, 255, 0.45f * a), UI_RGBA(255, 255, 255, 0), true);
        core_clip_pop(r);
        ui->animating = true;
    }
}

/* Scrolling: wheel + smoothing. Returns the drawn position. */
static f32 scroll_update(Ui *ui, Scroll *sc, vec2 pos, vec2 size, f32 content_h, f32 step) {
    const Platform_Input *in = ui->in;
    f32 max_scroll = CORE_MAX(0.0f, content_h - size.y);
    if (ui_mouse_in(ui, pos, size) && in->scroll_y != 0) {
        sc->target -= in->scroll_precise ? in->scroll_y * S(12) : in->scroll_y * step;
        sc->idle = 0;
        if (in->scroll_precise) sc->pos = CORE_CLAMP(sc->target, 0.0f, max_scroll);
    }
    sc->target = CORE_CLAMP(sc->target, 0.0f, max_scroll);
    f32 k = 1.0f - expf(-ui->dt * 20.0f);
    sc->pos += (sc->target - sc->pos) * k;
    if (fabsf(sc->target - sc->pos) < 0.25f) sc->pos = sc->target;
    else ui->animating = true;
    sc->pos = CORE_CLAMP(sc->pos, 0.0f, max_scroll);
    sc->idle += ui->dt;
    f32 show = (sc->idle < 0.9f || ui_mouse_in(ui, vec2_make(pos.x + size.x - S(24), pos.y), vec2_make(S(24), size.y))) ? 1.0f : 0.0f;
    sc->bar_t += ((max_scroll > 0 ? show : 0.0f) - sc->bar_t) * (1.0f - expf(-ui->dt * 10.0f));
    if (fabsf(sc->bar_t - (max_scroll > 0 ? show : 0.0f)) > 0.01f) ui->animating = true;
    return sc->pos;
}

static void scroll_bar(Ui *ui, const Scroll *sc, vec2 pos, vec2 size, f32 content_h) {
    f32 max_scroll = CORE_MAX(0.0f, content_h - size.y);
    if (max_scroll <= 0 || sc->bar_t < 0.01f) return;
    f32 bar_w = S(4), track_h = size.y - S(8);
    f32 thumb_h = CORE_MAX(S(28), track_h * size.y / content_h);
    f32 ty = pos.y + S(4) + (track_h - thumb_h) * (sc->pos / max_scroll);
    core_draw_rect_rounded(ui->r, vec2_make(pos.x + size.x - bar_w - S(2), ty), vec2_make(bar_w, thumb_h), bar_w * 0.5f,
                           UI_RGBA(255, 255, 255, 0.22f * sc->bar_t));
}

/* ------------------------------------------------------------------------- */
/* search state                                                              */
/* ------------------------------------------------------------------------- */

static void reset_results(Dl_View *v) {
    memset(v->checked, 0, sizeof(v->checked));
    memset(v->libstate, 0, sizeof(v->libstate));
    memset(v->seen_at, 0, sizeof(v->seen_at));
    v->seen_count = 0;
    v->sel = 0;
    v->scroll = (Scroll){0};
}

static void trimmed_query(const Dl_View *v, char *out, u32 cap) {
    u32 a = 0, n = v->query_len;
    while (a < n && v->query[a] == ' ') a++;
    while (n > a && v->query[n - 1] == ' ') n--;
    u32 len = CORE_MIN(n - a, cap - 1);
    memcpy(out, v->query + a, len);
    out[len] = 0;
}

static void start_search(Dl_View *v, b32 select_all) {
    char q[256];
    trimmed_query(v, q, sizeof(q));
    if (!q[0]) return;
    copy_str(v->searched, sizeof(v->searched), q);
    v->searched_kind = v->mode;
    v->has_searched = true;
    v->select_all_pending = select_all && v->mode == DL_QUERY_ARTIST;
    reset_results(v);
    downloads_search(v->d, v->mode, q);
    v->gen = downloads_search_info(v->d).generation;
}

static b32 query_is_stale(const Dl_View *v) {
    char q[256];
    trimmed_query(v, q, sizeof(q));
    return !v->has_searched || v->searched_kind != v->mode || strcmp(q, v->searched) != 0;
}

/* The artist a result is (presumably) by, for the "already in your library" check. */
static void result_artist(const Dl_View *v, const Dl_Result *r, char *out, u32 cap) {
    if (v->searched_kind == DL_QUERY_ARTIST) copy_str(out, cap, v->searched);
    else if (r->artist[0]) copy_str(out, cap, r->artist);
    else dl_clean_channel(r->channel, out, cap);
}

static Row_State row_state(Dl_View *v, const Library *lib, u32 i, const Dl_Result *r) {
    Dl_State st;
    if (downloads_has(v->d, r->vid, &st)) {
        switch (st) {
        case DL_QUEUED: return ROW_QUEUED;
        case DL_ACTIVE: return ROW_ACTIVE;
        case DL_DONE:   return ROW_DONE;
        default:        return ROW_FAILED;
        }
    }
    sync_lib_keys(v, lib);
    /* a YouTube Music hit whose artist is still being looked up can't be judged yet */
    b32 pending = r->music && !r->artist[0] && v->searched_kind != DL_QUERY_ARTIST;
    u64 rk = str_hash(r->vid) ^ (str_hash(r->title) * 31) ^ (str_hash(r->artist) * 131);
    if ((v->libstate[i] == 0 || v->libkey[i] != rk) && !pending) {
        char artist[256];
        result_artist(v, r, artist, sizeof(artist));
        v->libstate[i] = in_library(v, lib, artist, r) ? 2 : 1;
        v->libkey[i] = rk;
    } else if (pending) {
        v->libstate[i] = 0;
    }
    return v->libstate[i] == 2 ? ROW_LIB : ROW_FREE;
}

/* Results that can still be downloaded (not in the library, not queued). */
static b32 selectable(Row_State s) { return s == ROW_FREE || s == ROW_FAILED; }

typedef struct { u32 total, free_rows, selected; } Sel_Counts;

static Sel_Counts count_selection(Dl_View *v, const Library *lib, u32 count) {
    Sel_Counts c = { .total = count };
    for (u32 i = 0; i < count; i++) {
        Dl_Result r;
        if (!downloads_result(v->d, i, &r)) break;
        if (!selectable(row_state(v, lib, i, &r))) continue;
        c.free_rows++;
        c.selected += v->checked[i] != 0;
    }
    return c;
}

static u32 enqueue_checked(Dl_View *v, const Library *lib, u32 count, b32 only_sel_row) {
    Dl_Result picks[DL_MAX_RESULTS];
    u32 n = 0;
    for (u32 i = 0; i < count; i++) {
        Dl_Result r;
        if (!downloads_result(v->d, i, &r)) break;
        b32 want = only_sel_row ? (s32)i == v->sel : v->checked[i] != 0;
        if (!want || !selectable(row_state(v, lib, i, &r))) continue;
        picks[n++] = r;
        v->checked[i] = 0;
    }
    if (!n) return 0;
    return downloads_enqueue(v->d, picks, n, v->searched_kind == DL_QUERY_ARTIST ? v->searched : 0);
}

/* ------------------------------------------------------------------------- */
/* body states                                                               */
/* ------------------------------------------------------------------------- */

static b32 draw_missing_tools(Ui *ui, vec2 area, vec2 sz, f32 a, Dl_Tools tools) {
    Core_Renderer *r = ui->r;
    f32 cw = CORE_MIN(S(600), sz.x - S(24)), ch = S(330);
    vec2 p = vec2_make(area.x + (sz.x - cw) * 0.5f, area.y + CORE_MAX(S(8), (sz.y - ch) * 0.3f));
    glass(ui, p, vec2_make(cw, ch), S(18), a);
    vec2 tc = vec2_make(p.x + S(56), p.y + S(56));
    Core_BoxStyle tb = { .radius = S(16), .fill = UI_RGBA(255, 120, 150, 0.16f * a), .border = S(1), .border_color = UI_RGBA(255, 130, 160, 0.4f * a) };
    core_draw_box(r, vec2_make(tc.x - S(28), tc.y - S(28)), vec2_make(S(56), S(56)), &tb);
    ui_icon_warning(ui, tc, S(30), UI_RGBA(255, 140, 170, a));
    const char *title = !tools.ytdlp && !tools.ffmpeg ? "yt-dlp and ffmpeg are required"
                        : !tools.ytdlp ? "yt-dlp is required" : "ffmpeg is required";
    ui_text(ui, ui->font_semi, core_str(title), p.x + S(98), p.y + S(44), S(21), ui_alpha(UI_TEXT, a), UI_ALIGN_LEFT, cw - S(120));
    ui_text(ui, ui->font, core_str_lit("yt-dlp finds and fetches the songs, ffmpeg converts and tags them."),
            p.x + S(98), p.y + S(72), S(13.5f), ui_alpha(UI_TEXT_DIM, a), UI_ALIGN_LEFT, cw - S(120));
    /* what is found and what is not */
    f32 y = p.y + S(112);
    const char *names[2] = { "yt-dlp", "ffmpeg" };
    b32 ok[2] = { tools.ytdlp, tools.ffmpeg };
    for (u32 i = 0; i < 2; i++) {
        f32 x = p.x + S(32) + (f32)i * S(150);
        if (ok[i]) ui_icon_check(ui, vec2_make(x + S(8), y), S(18), 1, ui_alpha(UI_ACCENT_BRIGHT, a));
        else ui_icon_close(ui, vec2_make(x + S(8), y), S(15), UI_RGBA(255, 140, 170, a));
        ui_text(ui, ui->font_med, core_str(names[i]), x + S(24), y, S(14.5f), ui_alpha(ok[i] ? UI_TEXT : UI_RGBA(255, 170, 190, 1), a), UI_ALIGN_LEFT, 0);
    }
    /* install hints */
    static const char *const cmds[3][2] = {
        { "Arch", "sudo pacman -S yt-dlp ffmpeg" },
        { "Debian", "sudo apt install ffmpeg && pipx install yt-dlp" },
        { "Any", "pip install -U yt-dlp" },
    };
    for (u32 i = 0; i < 3; i++) {
        f32 cy = p.y + S(156) + (f32)i * S(38);
        Core_BoxStyle cb = { .radius = S(9), .fill = UI_RGBA(0, 0, 0, 0.26f * a), .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.07f * a) };
        core_draw_box(r, vec2_make(p.x + S(32), cy - S(15)), vec2_make(cw - S(64), S(30)), &cb);
        ui_text(ui, ui->font, core_str(cmds[i][0]), p.x + S(46), cy, S(12.5f), ui_alpha(UI_TEXT_FAINT, a), UI_ALIGN_LEFT, 0);
        ui_text(ui, ui->font_mono, core_str(cmds[i][1]), p.x + S(112), cy, S(12.5f), ui_alpha(UI_RGBA(210, 206, 228, 1), a), UI_ALIGN_LEFT, cw - S(150));
    }
    Core_String label = core_str_lit("Check again");
    f32 bw = ui_text_width(ui, ui->font_med, label, S(14)) + S(56), bh = S(38);
    return pill_button(ui, ui_id("dv.recheck"), label, vec2_make(p.x + cw - S(32) - bw, p.y + ch - S(28) - bh), vec2_make(bw, bh),
                       true, true, a, ui_icon_retry).clicked;
}

static void draw_skeleton(Ui *ui, vec2 area, vec2 sz, f32 a, f32 row_h) {
    ui->animating = true;
    u32 n = (u32)(sz.y / row_h);
    for (u32 i = 0; i < n && i < 9; i++) {
        f32 y = area.y + (f32)i * row_h;
        f32 wave = 0.5f + 0.5f * sinf((f32)ui->time * 3.0f - (f32)i * 0.5f);
        f32 fade = a * (1.0f - (f32)i / 10.0f);
        vec4 c = UI_RGBA(255, 255, 255, (0.04f + 0.035f * wave) * fade);
        core_draw_rect_rounded(ui->r, vec2_make(area.x + S(58), y + (row_h - S(44)) * 0.5f), vec2_make(S(44), S(44)), S(8), c);
        core_draw_rect_rounded(ui->r, vec2_make(area.x + S(114), y + row_h * 0.5f - S(14)), vec2_make(S(150 + (i * 37) % 120), S(11)), S(5), c);
        core_draw_rect_rounded(ui->r, vec2_make(area.x + S(114), y + row_h * 0.5f + S(6)), vec2_make(S(90 + (i * 23) % 70), S(9)), S(4), c);
    }
}

/* ---- result rows ---- */

/* YouTube's own thumbnail when it has arrived (fading in over the placeholder). */
static void draw_thumb(Dl_View *v, Ui *ui, const Dl_Result *r, vec2 p, f32 size, f32 a) {
    Ythumb th = ythumbs_get(v->thumbs, r->vid, ui->dt);
    f32 fade = th.ready ? ease_out_cubic(th.age / 0.3f) : 0;
    if (th.ready && th.age < 0.35f) ui->animating = true;
    if (fade < 1) {
        u64 h = str_hash(r->vid);
        Core_BoxStyle ph = { .radius = S(8), .fill = hash_color(h, a), .fill2 = UI_RGBA(24, 20, 40, a), .gradient = 1 };
        core_draw_box(ui->r, p, vec2_make(size, size), &ph);
        ui_icon_note(ui, vec2_make(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.42f, UI_RGBA(255, 255, 255, 0.3f * a));
    }
    if (th.ready)
        core_draw_image_rounded(ui->r, th.tex, p, vec2_make(size, size), th.uv0, th.uv1, S(8), -1, (vec4){ .x = 1, .y = 1, .z = 1, .w = fade * a });
}

typedef struct { b32 clicked; b32 hovered; } Row_Out;

/* Right-hand status of a row: what is happening with this song. */
static void draw_row_state(Dl_View *v, Ui *ui, const Dl_Result *res, Row_State st, vec2 rp, vec2 rs, f32 a) {
    f32 cy = rp.y + rs.y * 0.5f, xr = rp.x + rs.x - S(20);
    if (st == ROW_LIB) {
        ui_icon_check(ui, vec2_make(xr - S(8), cy), S(18), 1, ui_alpha(UI_TEXT_DIM, a));
        ui_text(ui, ui->font, core_str_lit("In library"), xr - S(24), cy, S(13), ui_alpha(UI_TEXT_DIM, a), UI_ALIGN_RIGHT, 0);
    } else if (st == ROW_QUEUED) {
        spinner(ui, vec2_make(xr - S(8), cy), S(8), S(2), ui_alpha(UI_LAVENDER, a * 0.8f));
        ui_text(ui, ui->font, core_str_lit("Queued"), xr - S(24), cy, S(13), ui_alpha(UI_LAVENDER, a), UI_ALIGN_RIGHT, 0);
    } else if (st == ROW_ACTIVE) {
        spinner(ui, vec2_make(xr - S(8), cy), S(8), S(2), ui_alpha(UI_ACCENT_BRIGHT, a));
        ui_text(ui, ui->font, core_str_lit("Downloading"), xr - S(24), cy, S(13), ui_alpha(UI_ACCENT_BRIGHT, a), UI_ALIGN_RIGHT, 0);
    } else if (st == ROW_DONE) {
        ui_icon_check(ui, vec2_make(xr - S(8), cy), S(19), 1, ui_alpha(UI_ACCENT_BRIGHT, a));
        ui_text(ui, ui->font, core_str_lit("Saved"), xr - S(24), cy, S(13), ui_alpha(UI_ACCENT_BRIGHT, a), UI_ALIGN_RIGHT, 0);
    } else if (st == ROW_FAILED) {
        ui_icon_warning(ui, vec2_make(xr - S(8), cy), S(18), UI_RGBA(255, 140, 170, a));
        ui_text(ui, ui->font, core_str_lit("Failed, click to retry"), xr - S(24), cy, S(13), UI_RGBA(255, 160, 182, a), UI_ALIGN_RIGHT, 0);
    }
    CORE_UNUSED(v);
}

/* Returns true when the "Download N songs" button was pressed. */
static b32 draw_results(Dl_View *v, Ui *ui, const Dlv_Frame *f, const Dl_SearchInfo *info, vec2 area, vec2 sz, f32 a,
                        b32 live, f32 bottom_pad) {
    Core_Renderer *r = ui->r;
    const Platform_Input *in = ui->in;
    b32 artist_mode = v->searched_kind == DL_QUERY_ARTIST;
    u32 count = info->count;
    Sel_Counts sc = count_selection(v, f->lib, count);
    b32 download_clicked = false;

    /* ---- header line: select all / count, and the download button ---- */
    f32 hh = S(46);
    f32 hy = area.y + hh * 0.5f;
    if (!count && info->state == DL_SEARCH_BUSY) {
        char buf[320];
        snprintf(buf, sizeof(buf), artist_mode ? "Finding the top songs by \xE2\x80\x9C%s\xE2\x80\x9D\xE2\x80\xA6" : "Searching YouTube for \xE2\x80\x9C%s\xE2\x80\x9D\xE2\x80\xA6", v->searched);
        ui_text(ui, ui->font_med, core_str(buf), area.x + S(10), hy, S(14.5f), ui_alpha(UI_TEXT_DIM, a), UI_ALIGN_LEFT, sz.x - S(20));
    } else if (artist_mode && sc.free_rows == 0 && count > 0) {
        /* everything is already queued, saved or in the library */
        ui_icon_check(ui, vec2_make(area.x + S(28), hy), S(20), 1, ui_alpha(UI_ACCENT_BRIGHT, a));
        ui_text(ui, ui->font_med, core_str_lit("All of these are queued, saved or already in your library"), area.x + S(52), hy, S(14.5f),
                ui_alpha(UI_TEXT_DIM, a), UI_ALIGN_LEFT, sz.x - S(70));
    } else if (artist_mode) {
        u64 id = ui_id("dv.all");
        b32 saved = ui->input_enabled;
        ui->input_enabled = saved && live;
        Ui_Interact it = ui_interact(ui, id, vec2_make(area.x, area.y + S(4)), vec2_make(S(250), hh - S(8)), PLATFORM_CURSOR_HAND);
        ui->input_enabled = saved;
        b32 all = sc.free_rows > 0 && sc.selected == sc.free_rows;
        b32 some = sc.selected > 0 && !all;
        if (it.hover_t > 0.01f) core_draw_rect_rounded(r, vec2_make(area.x, area.y + S(4)), vec2_make(S(250), hh - S(8)), S(9), UI_RGBA(255, 255, 255, 0.05f * it.hover_t * a));
        checkbox(ui, id, vec2_make(area.x + S(28), hy), S(22), all, some, it.hover_t, a);
        char buf[96];
        snprintf(buf, sizeof(buf), "Select all %u", sc.free_rows);
        ui_text(ui, ui->font_med, core_str(buf), area.x + S(52), hy, S(14.5f), ui_alpha(UI_TEXT, a), UI_ALIGN_LEFT, S(190));
        if (it.clicked && sc.free_rows) {
            for (u32 i = 0; i < count; i++) {
                Dl_Result res;
                if (!downloads_result(v->d, i, &res)) break;
                v->checked[i] = selectable(row_state(v, f->lib, i, &res)) ? (all ? 0 : 1) : 0;
            }
            ui_anim_kick(ui, ui_idx(id, 1), 6.0f);
        }
        if (live && in->ctrl && in->key_pressed[PLATFORM_KEY_A]) {
            for (u32 i = 0; i < count; i++) {
                Dl_Result res;
                if (!downloads_result(v->d, i, &res)) break;
                v->checked[i] = selectable(row_state(v, f->lib, i, &res)) ? 1 : 0;
            }
        }

        /* download button: grows in once something is ticked */
        char lbl[64];
        snprintf(lbl, sizeof(lbl), sc.selected == 1 ? "Download 1 song" : "Download %u songs", sc.selected);
        Core_String label = core_str(lbl);
        f32 bw = ui_text_width(ui, ui->font_med, label, S(14)) + S(62), bh = S(36);
        f32 appear = ui_spring(ui, ui_id("dv.dlbtn"), sc.selected ? 1.0f : 0.0f, 380, 26);
        f32 sc_a = clamp01(appear);
        if (appear > 0.01f) {
            vec2 bp = vec2_make(area.x + sz.x - bw - S(8) + (1 - sc_a) * S(14), hy - bh * 0.5f);
            download_clicked = pill_button(ui, ui_id("dv.download"), label, bp, vec2_make(bw, bh), true, sc.selected > 0 && live,
                                           a * sc_a, ui_icon_download).clicked;
        }
        if (appear <= 0.01f) {
            ui_text(ui, ui->font, core_str_lit("Nothing selected"), area.x + sz.x - S(8), hy, S(13), ui_alpha(UI_TEXT_FAINT, a), UI_ALIGN_RIGHT, 0);
        }
    } else {
        char buf[320];
        snprintf(buf, sizeof(buf), "%u result%s for \xE2\x80\x9C%s\xE2\x80\x9D", count, count == 1 ? "" : "s", v->searched);
        ui_text(ui, ui->font_med, core_str(buf), area.x + S(10), hy, S(14.5f), ui_alpha(UI_TEXT, a), UI_ALIGN_LEFT, sz.x - S(260));
        ui_text(ui, ui->font, core_str_lit("Click a version to download it"), area.x + sz.x - S(10), hy, S(13),
                ui_alpha(UI_TEXT_FAINT, a), UI_ALIGN_RIGHT, 0);
    }
    core_draw_rect(r, vec2_make(area.x, area.y + hh), vec2_make(sz.x, S(1)), UI_RGBA(255, 255, 255, 0.07f * a));

    /* ---- list ---- */
    f32 row_h = S(62);
    vec2 lp = vec2_make(area.x, area.y + hh + S(4));
    vec2 ls = vec2_make(sz.x, area.y + sz.y - lp.y);
    f32 content = row_h * (f32)count + bottom_pad + S(8);

    /* keyboard: up/down move the highlight, which scrolls into view */
    if (live && count) {
        if (in->key_pressed[PLATFORM_KEY_DOWN]) v->sel = CORE_MIN(v->sel + 1, (s32)count - 1);
        if (in->key_pressed[PLATFORM_KEY_UP]) v->sel = CORE_MAX(v->sel - 1, 0);
        if (in->key_pressed[PLATFORM_KEY_DOWN] || in->key_pressed[PLATFORM_KEY_UP]) {
            f32 top = (f32)v->sel * row_h, bot = top + row_h;
            if (top < v->scroll.target) v->scroll.target = top;
            if (bot > v->scroll.target + ls.y - bottom_pad) v->scroll.target = bot - ls.y + bottom_pad;
            v->scroll.idle = 0;
        }
    }
    v->sel = CORE_CLAMP(v->sel, 0, count ? (s32)count - 1 : 0);
    /* The floating dock blocks wheel input as well as row interactions. */
    b32 saved_in = ui->input_enabled;
    ui->input_enabled = saved_in && live;
    f32 pos = scroll_update(ui, &v->scroll, lp, ls, content, row_h * 2.0f);
    u32 first = (u32)CORE_MAX(0.0f, floorf(pos / row_h));
    u32 last = CORE_MIN(count, first + (u32)(ls.y / row_h) + 2);
    core_clip_push(r, lp, ls);

    f32 sel_y = ui_spring(ui, ui_id("dv.sel"), lp.y - pos + (f32)v->sel * row_h, 600, 44);
    if (count) {
        Core_BoxStyle sb = { .radius = S(11), .fill = UI_RGBA(255, 255, 255, 0.05f * a),
                             .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.07f * a) };
        core_draw_box(r, vec2_make(lp.x, sel_y + S(1)), vec2_make(sz.x - S(10), row_h - S(2)), &sb);
    }
    for (u32 i = first; i < last; i++) {
        Dl_Result res;
        if (!downloads_result(v->d, i, &res)) break;
        if (v->seen_at[i] == 0) v->seen_at[i] = ui->time;
        f32 e = ease_out_cubic((f32)((ui->time - v->seen_at[i]) / 0.38));
        if (e < 1) ui->animating = true;
        Row_State st = row_state(v, f->lib, i, &res);
        f32 ra = a * e;
        f32 y = lp.y - pos + (f32)i * row_h;
        vec2 rp = vec2_make(lp.x + S(14) * (1 - e), y + S(1)), rs = vec2_make(sz.x - S(10), row_h - S(2));
        u64 id = ui_idx(ui_id("dv.row"), str_hash(res.vid));
        Ui_Interact it = ui_interact(ui, id, rp, rs, selectable(st) || st == ROW_FAILED ? PLATFORM_CURSOR_HAND : PLATFORM_CURSOR_DEFAULT);
        if (it.hovered && (in->mouse_delta.x != 0 || in->mouse_delta.y != 0)) v->sel = (s32)i;
        if (it.hover_t > 0.01f && selectable(st)) core_draw_rect_rounded(r, rp, rs, S(11), UI_RGBA(255, 255, 255, 0.03f * it.hover_t * ra));
        if (it.pressed && selectable(st)) ui_ripple(ui, in->mouse_pos, rs.x * 0.5f, UI_RGBA(160, 130, 255, 0.16f), rp, rs);
        f32 dim = st == ROW_LIB ? 0.55f : 1.0f;

        f32 tx = rp.x + S(14);
        if (artist_mode) {
            checkbox(ui, ui_idx(id, 2), vec2_make(rp.x + S(28), rp.y + rs.y * 0.5f), S(22),
                     v->checked[i] && selectable(st), false, it.hover_t, ra * (selectable(st) ? 1.0f : 0.35f));
            tx = rp.x + S(54);
        }
        f32 th = S(44);
        draw_thumb(v, ui, &res, vec2_make(tx, rp.y + (rs.y - th) * 0.5f), th, ra * dim);
        f32 textx = tx + th + S(14);
        f32 right_w = S(150);
        f32 max_w = rs.x - (textx - rp.x) - right_w;
        char artist[256];
        result_artist(v, &res, artist, sizeof(artist));
        /* the raw title: "(Official Video)" vs "(Lyrics)" is how you tell versions apart */
        ui_text(ui, ui->font, core_str(res.title), textx, rp.y + rs.y * 0.5f - S(10), S(15.5f), ui_alpha(UI_RGBA(232, 229, 244, 1), ra * dim), UI_ALIGN_LEFT, max_w);
        char meta[320];
        u32 m = 0;
        f32 meta_x = textx;
        if (res.music && !artist_mode) {
            /* a studio version from YouTube Music: a small tag, then the rest */
            Core_String tag = core_str_lit("SONG");
            f32 tw = ui_text_width(ui, ui->font_semi, tag, S(10.5f));
            vec2 tp = vec2_make(textx, rp.y + rs.y * 0.5f + S(12) - S(8.5f));
            Core_BoxStyle tb = { .radius = S(5), .fill = UI_RGBA(139, 102, 255, 0.22f * ra * dim), .border = S(1), .border_color = UI_RGBA(160, 130, 255, 0.4f * ra * dim) };
            core_draw_box(r, tp, vec2_make(tw + S(12), S(17)), &tb);
            ui_text(ui, ui->font_semi, tag, tp.x + S(6), tp.y + S(8.5f), S(10.5f), ui_alpha(UI_ACCENT_BRIGHT, ra * dim), UI_ALIGN_LEFT, 0);
            meta_x += tw + S(20);
            m += (u32)snprintf(meta + m, sizeof(meta) - m, "YouTube Music");
        }
        if (artist[0]) m += (u32)snprintf(meta + m, sizeof(meta) - m, "%s%s", m ? "  \xC2\xB7  " : "", artist);
        char tmp[48];
        if (res.duration_s) { fmt_duration(tmp, sizeof(tmp), res.duration_s); m += (u32)snprintf(meta + m, sizeof(meta) - m, "%s%s", m ? "  \xC2\xB7  " : "", tmp); }
        if (res.views && !artist_mode) { fmt_views(tmp, sizeof(tmp), res.views); m += (u32)snprintf(meta + m, sizeof(meta) - m, "%s%s", m ? "  \xC2\xB7  " : "", tmp); }
        if (artist_mode && !res.duration_s) m += (u32)snprintf(meta + m, sizeof(meta) - m, "  \xC2\xB7  #%u", i + 1);
        meta[m] = 0;
        ui_text(ui, ui->font, core_str(meta), meta_x, rp.y + rs.y * 0.5f + S(12), S(13), ui_alpha(UI_TEXT_DIM, ra * dim), UI_ALIGN_LEFT, max_w - (meta_x - textx));

        if (st == ROW_FREE && !artist_mode) {
            /* download button, lights up with the row */
            vec2 bc = vec2_make(rp.x + rs.x - S(30), rp.y + rs.y * 0.5f);
            f32 hv = CORE_MAX(it.hover_t, (s32)i == v->sel ? 0.35f : 0.0f);
            f32 br = S(17) * (1 + 0.06f * hv - 0.08f * it.press_t);
            Core_BoxStyle bb = { .radius = br, .fill = ui_alpha(ui_mix(UI_RGBA(255, 255, 255, 0.07f), UI_RGBA(128, 98, 236, 1), hv), ra),
                                 .border = S(1), .border_color = UI_RGBA(255, 255, 255, (0.10f + 0.12f * hv) * ra) };
            core_draw_box(r, vec2_make(bc.x - br, bc.y - br), vec2_make(br * 2, br * 2), &bb);
            ui_icon_download(ui, bc, S(17), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, hv), ra));
        } else if (st != ROW_FREE) {
            draw_row_state(v, ui, &res, st, rp, rs, ra);
        }

        if (it.clicked) {
            if (artist_mode && selectable(st)) {
                v->checked[i] = !v->checked[i];
            } else if (!artist_mode && selectable(st)) {
                v->sel = (s32)i;
                if (enqueue_checked(v, f->lib, count, true)) {
                    v->dock_flash_until = ui->time + 2.6;
                    ui_burst(ui, vec2_make(rp.x + rs.x - S(30), rp.y + rs.y * 0.5f), 14, UI_RGBA(190, 160, 255, 1), 0, S(120));
                }
            }
        }
    }
    ui->input_enabled = saved_in;
    if (!count && info->state == DL_SEARCH_BUSY) draw_skeleton(ui, lp, ls, a, row_h);
    core_clip_pop(r);
    scroll_bar(ui, &v->scroll, lp, ls, content);

    if (info->state == DL_SEARCH_BUSY && count) ui->animating = true; /* more results are streaming in */

    return download_clicked;
}

/* ------------------------------------------------------------------------- */
/* dock                                                                      */
/* ------------------------------------------------------------------------- */

static const char *phase_text(const Dl_Job *j, char *buf, u32 cap) {
    switch (j->state) {
    case DL_QUEUED: return j->attempts ? "Retrying soon" : "Waiting";
    case DL_DONE:   return j->existed ? "Already in your library" : "Saved";
    case DL_FAILED: return j->error[0] ? j->error : "Failed";
    default: break;
    }
    switch (j->phase) {
    case DL_PHASE_FETCH:   snprintf(buf, cap, "Downloading %u%%", (u32)(CORE_CLAMP((j->progress - 0.05f) / 0.80f, 0.0f, 1.0f) * 100.0f)); return buf;
    case DL_PHASE_CONVERT: return "Converting to MP3";
    case DL_PHASE_TAG:     return "Adding tags and cover";
    default:               return "Starting";
    }
}

/* Builds the display order: running, failed, waiting, then finished (newest first). */
static u32 build_order(Dl_View *v, u32 n) {
    u32 w = 0;
    static const u8 rank_states[4] = { DL_ACTIVE, DL_FAILED, DL_QUEUED, DL_DONE };
    for (u32 k = 0; k < 4; k++) {
        if (rank_states[k] == DL_DONE) {
            for (u32 i = n; i-- > 0;) if (v->briefs[i].state == DL_DONE) v->order[w++] = i;
        } else {
            for (u32 i = 0; i < n; i++) if (v->briefs[i].state == rank_states[k]) v->order[w++] = i;
        }
    }
    return w;
}

static void draw_job_row(Dl_View *v, Ui *ui, const Dl_Job *j, vec2 rp, vec2 rs, f32 a, b32 live) {
    Core_Renderer *r = ui->r;
    u64 id = ui_idx(ui_id("dv.job"), j->uid);
    Ui_Interact it = ui_interact(ui, id, rp, rs, PLATFORM_CURSOR_DEFAULT);
    if (it.hover_t > 0.01f) core_draw_rect_rounded(r, rp, rs, S(10), UI_RGBA(255, 255, 255, 0.045f * it.hover_t * a));
    vec2 gc = vec2_make(rp.x + S(28), rp.y + rs.y * 0.5f);
    f32 gr = S(15);
    char ph[32];
    const char *status = phase_text(j, ph, sizeof(ph));
    vec4 scol = UI_TEXT_DIM;
    if (j->state == DL_ACTIVE) {
        ring(ui, gc, gr, S(2.6f), CORE_CLAMP(j->progress, 0.03f, 1.0f), UI_ACCENT_BRIGHT, a);
        if (j->phase == DL_PHASE_FETCH) ui_icon_download(ui, gc, S(14), ui_alpha(UI_TEXT, a));
        else { spinner(ui, gc, S(6), S(2), ui_alpha(UI_LAVENDER, a)); }
        scol = UI_ACCENT_BRIGHT;
    } else if (j->state == DL_QUEUED) {
        core_draw_circle_ex(ui->r, gc, gr, S(1.6f), 0, UI_RGBA(255, 255, 255, 0.14f * a));
        for (s32 k = -1; k <= 1; k++) core_draw_circle(ui->r, vec2_make(gc.x + (f32)k * S(5), gc.y), S(1.7f), UI_RGBA(255, 255, 255, 0.45f * a));
    } else if (j->state == DL_DONE) {
        f32 pop = ui_spring(ui, ui_idx(id, 3), 1.0f, 300, 18);
        Core_BoxStyle ok = { .radius = gr, .fill = UI_RGBA(128, 98, 236, 0.9f * a) };
        core_draw_box(ui->r, vec2_make(gc.x - gr, gc.y - gr), vec2_make(gr * 2, gr * 2), &ok);
        ui_icon_check(ui, gc, S(16) * CORE_MAX(pop, 0.0f), 1, ui_alpha(UI_TEXT, a));
        scol = UI_LAVENDER;
    } else {
        core_draw_circle(ui->r, gc, gr, UI_RGBA(255, 110, 140, 0.16f * a));
        ui_icon_warning(ui, gc, S(17), UI_RGBA(255, 140, 170, a));
        scol = UI_RGBA(255, 160, 182, 1);
    }
    f32 tx = rp.x + S(58);
    f32 actions_w = it.hovered ? S(82) : S(0);
    f32 max_w = rs.x - S(58) - S(16) - actions_w;
    char sub[DL_TEXT * 2];
    snprintf(sub, sizeof(sub), "%s%s%s", j->artist, j->artist[0] ? "  \xC2\xB7  " : "", status);
    ui_text(ui, ui->font, core_str(j->title[0] ? j->title : j->vid), tx, rp.y + rs.y * 0.5f - S(9), S(14.5f), ui_alpha(UI_RGBA(232, 229, 244, 1), a), UI_ALIGN_LEFT, max_w);
    ui_text(ui, ui->font, core_str(sub), tx, rp.y + rs.y * 0.5f + S(11), S(12.5f), ui_alpha(scol, a), UI_ALIGN_LEFT, max_w);
    if (j->state == DL_ACTIVE) {
        bar(ui, vec2_make(tx, rp.y + rs.y - S(6)), vec2_make(rs.x - S(58) - S(16), S(3)), j->progress, a, false);
    }
    /* hover actions: retry (failed) and remove */
    if (it.hovered && live) {
        vec2 xc = vec2_make(rp.x + rs.x - S(26), rp.y + rs.y * 0.5f);
        Ui_Interact rm = round_button(ui, ui_idx(id, 5), xc, S(15), a);
        ui_icon_close(ui, xc, S(14), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, rm.hover_t), a));
        if (rm.clicked) downloads_remove(v->d, j->uid);
        if (j->state == DL_FAILED) {
            vec2 rc = vec2_make(xc.x - S(34), xc.y);
            Ui_Interact rt = round_button(ui, ui_idx(id, 6), rc, S(15), a);
            ui_icon_retry(ui, rc, S(16), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, rt.hover_t), a));
            if (rt.clicked) downloads_retry(v->d, j->uid);
        }
    }
}

static void draw_dock(Dl_View *v, Ui *ui, vec2 win, f32 vis, b32 live, const Dl_Summary *sum) {
    Core_Renderer *r = ui->r;
    /* nothing running, nothing to retry, nothing new: songs finished in an earlier session need no dock */
    if (!sum->total || (!sum->queued && !sum->active && !sum->failed && !sum->session_done)) {
        v->dock_t = 0;
        v->dock_size = vec2_zero();
        return;
    }

    f32 pill_h = S(60);
    f32 w = CORE_MIN(S(780), win.x - S(48));
    f32 max_h = CORE_MIN(S(470), win.y * 0.62f);
    /* as tall as the list needs, up to the maximum */
    f32 full_h = pill_h + CORE_CLAMP(S(56) * (f32)sum->total + S(20), S(76), CORE_MAX(S(76), max_h - pill_h));
    f32 h = core_lerp(pill_h, full_h, clamp01(v->dock_t));
    /* slide up when it first appears */
    f32 appear = ui_spring(ui, ui_id("dv.dock.in"), 1.0f, 300, 26);
    vec2 p = vec2_make((win.x - w) * 0.5f, win.y - S(24) - h + (1 - appear) * S(60));
    vec2 sz = vec2_make(w, h);
    v->dock_pos = p;
    v->dock_size = sz;

    /* hover/pin/flash decide whether it is open */
    b32 hover = ui_mouse_in(ui, p, sz);
    b32 flash = ui->time < v->dock_flash_until;
    v->dock_t = ui_spring(ui, ui_id("dv.dock.t"), (v->dock_pinned || flash || hover) ? 1.0f : 0.0f, 360, 30);
    if (v->dock_t < 0) v->dock_t = 0;
    if (flash) ui->animating = true;

    f32 a = vis * clamp01(appear);
    f32 glow = flash ? 0.5f + 0.5f * sinf((f32)ui->time * 8.0f) : 0;
    if (glow > 0) {
        core_set_blend(r, CORE_BLEND_ADD);
        core_draw_shadow(r, p, sz, S(18), S(30), UI_RGBA(150, 110, 255, 0.35f * glow * a));
        core_set_blend(r, CORE_BLEND_NORMAL);
    }
    glass(ui, p, sz, S(18), a);
    core_clip_push(r, p, sz);

    /* ---- job list (above the pill row) ---- */
    f32 list_h = h - pill_h;
    f32 expand = clamp01((v->dock_t - 0.08f) / 0.5f);
    if (list_h > S(14) && expand > 0.01f) {
        u32 n = downloads_briefs(v->d, v->briefs, MAX_JOBS_SHOWN);
        u32 shown = build_order(v, n);
        f32 row_h = S(56);
        vec2 lp = vec2_make(p.x + S(10), p.y + S(8)), ls = vec2_make(w - S(20), list_h - S(12));
        f32 content = row_h * (f32)shown + S(4);
        f32 pos = scroll_update(ui, &v->dock_scroll, lp, ls, content, row_h * 2.0f);
        core_clip_push(r, lp, ls);
        u32 first = (u32)CORE_MAX(0.0f, floorf(pos / row_h));
        u32 last = CORE_MIN(shown, first + (u32)(ls.y / row_h) + 2);
        b32 saved = ui->input_enabled;
        for (u32 k = first; k < last; k++) {
            u32 bi = v->order[k];
            Dl_Job j;
            if (!downloads_job(v->d, bi, &j) || j.uid != v->briefs[bi].uid) continue;
            f32 y = lp.y - pos + (f32)k * row_h;
            /* rows only react while their middle is inside the visible list */
            b32 inside = y + row_h * 0.5f > lp.y && y + row_h * 0.5f < lp.y + ls.y;
            ui->input_enabled = saved && live && inside;
            draw_job_row(v, ui, &j, vec2_make(lp.x, y), vec2_make(ls.x - S(8), row_h - S(4)), a * expand, ui->input_enabled);
        }
        ui->input_enabled = saved;
        core_clip_pop(r);
        scroll_bar(ui, &v->dock_scroll, lp, ls, content);
        core_draw_rect(r, vec2_make(p.x + S(16), p.y + list_h - S(1)), vec2_make(w - S(32), S(1)), UI_RGBA(255, 255, 255, 0.07f * a * expand));
    }

    /* ---- pill row ---- */
    f32 py = p.y + h - pill_h;
    f32 cy = py + pill_h * 0.5f;
    u32 pending = sum->queued + sum->active;
    vec2 ic = vec2_make(p.x + S(36), cy);
    b32 all_done = pending == 0;
    if (!all_done) {
        ring(ui, ic, S(17), S(3), sum->progress, UI_ACCENT_BRIGHT, a);
        if (sum->active) ui_icon_download(ui, vec2_make(ic.x, ic.y + S(0.5f)), S(16), ui_alpha(UI_TEXT, a));
        else ui_icon_pause(ui, ic, S(13), ui_alpha(UI_TEXT, a));
        if (sum->active) ui->animating = true;
    } else {
        f32 pop = ui_spring(ui, ui_id("dv.dock.pop"), 1.0f, 300, 18);
        Core_BoxStyle ok = { .radius = S(17), .fill = sum->failed && !sum->done ? UI_RGBA(255, 110, 140, 0.2f * a) : UI_RGBA(128, 98, 236, 0.92f * a) };
        core_draw_box(r, vec2_make(ic.x - S(17), ic.y - S(17)), vec2_make(S(34), S(34)), &ok);
        if (sum->failed && !sum->done) ui_icon_warning(ui, ic, S(19), UI_RGBA(255, 140, 170, a));
        else ui_icon_check(ui, ic, S(19) * CORE_MAX(pop, 0.0f), 1, ui_alpha(UI_TEXT, a));
    }
    char l1[96], l2[DL_TEXT * 3] = {0};
    if (sum->paused && pending) snprintf(l1, sizeof(l1), "Paused  \xC2\xB7  %u waiting", pending);
    else if (pending) snprintf(l1, sizeof(l1), "Downloading  \xC2\xB7  %u of %u", sum->batch_done + (sum->active ? 1 : 0) > sum->batch_total ? sum->batch_total : sum->batch_done + (sum->active ? 1 : 0), sum->batch_total);
    else if (sum->failed) snprintf(l1, sizeof(l1), "%u saved  \xC2\xB7  %u failed", sum->batch_done > sum->failed ? sum->batch_done - sum->failed : 0, sum->failed);
    else snprintf(l1, sizeof(l1), sum->batch_total == 1 ? "1 song saved" : "%u songs saved", sum->batch_total ? sum->batch_total : sum->done);
    /* second line: the song being worked on */
    if (pending) {
        u32 n = downloads_briefs(v->d, v->briefs, MAX_JOBS_SHOWN);
        for (u32 i = 0; i < n; i++) {
            if (v->briefs[i].state != DL_ACTIVE) continue;
            Dl_Job j;
            if (downloads_job(v->d, i, &j) && j.uid == v->briefs[i].uid) {
                char ph[32];
                snprintf(l2, sizeof(l2), "%s%s%s  \xC2\xB7  %s", j.artist, j.artist[0] ? " \xE2\x80\x94 " : "", j.title, phase_text(&j, ph, sizeof(ph)));
            }
            break;
        }
    } else {
        snprintf(l2, sizeof(l2), v->dock_t > 0.5f ? "They are in your library" : "Hover to see every song");
    }
    f32 btn_w = S(150);
    f32 text_w = w - S(72) - btn_w - S(20);
    ui_text(ui, ui->font_med, core_str(l1), p.x + S(66), cy - S(10), S(15), ui_alpha(UI_TEXT, a), UI_ALIGN_LEFT, text_w);
    ui_text(ui, ui->font, core_str(l2), p.x + S(66), cy + S(11), S(12.5f), ui_alpha(UI_TEXT_DIM, a), UI_ALIGN_LEFT, text_w);

    /* buttons: pause/resume, retry failed, clear finished, expand */
    f32 bx = p.x + w - S(32);
    {
        Ui_Interact it = round_button(ui, ui_id("dv.dock.pin"), vec2_make(bx, cy), S(17), a);
        if (v->dock_t > 0.5f) ui_icon_chevron_down(ui, vec2_make(bx, cy), S(18), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, it.hover_t), a));
        else ui_icon_chevron_up(ui, vec2_make(bx, cy), S(18), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, it.hover_t), a));
        if (it.clicked) v->dock_pinned = !v->dock_pinned;
        bx -= S(38);
    }
    if (sum->done + sum->failed > 0 && v->dock_t > 0.3f) {
        Ui_Interact it = round_button(ui, ui_id("dv.dock.clear"), vec2_make(bx, cy), S(17), a * expand);
        ui_icon_close(ui, vec2_make(bx, cy), S(15), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, it.hover_t), a * expand));
        if (it.clicked) downloads_clear_finished(v->d);
        bx -= S(38);
    }
    if (sum->failed > 0) {
        Ui_Interact it = round_button(ui, ui_id("dv.dock.retry"), vec2_make(bx, cy), S(17), a);
        ui_icon_retry(ui, vec2_make(bx, cy), S(18), ui_alpha(ui_mix(UI_RGBA(255, 160, 182, 1), UI_TEXT, it.hover_t), a));
        if (it.clicked) downloads_retry_failed(v->d);
        bx -= S(38);
    }
    if (pending > 1 && v->dock_t > 0.3f) {
        /* stop everything that is waiting */
        Ui_Interact it = round_button(ui, ui_id("dv.dock.stop"), vec2_make(bx, cy), S(17), a * expand);
        f32 sq = S(11);
        core_draw_rect_rounded(r, vec2_make(bx - sq * 0.5f, cy - sq * 0.5f), vec2_make(sq, sq), S(2.5f),
                               ui_alpha(ui_mix(UI_TEXT_DIM, UI_RGBA(255, 160, 182, 1), it.hover_t), a * expand));
        if (it.clicked) downloads_cancel_all(v->d);
        bx -= S(38);
    }
    if (pending > 0) {
        Ui_Interact it = round_button(ui, ui_id("dv.dock.pause"), vec2_make(bx, cy), S(17), a);
        vec4 col = ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, it.hover_t), a);
        if (sum->paused) ui_icon_play(ui, vec2_make(bx, cy), S(16), col);
        else ui_icon_pause(ui, vec2_make(bx, cy), S(15), col);
        if (it.clicked) downloads_set_paused(v->d, !sum->paused);
        bx -= S(38);
    }
    /* overall progress along the pill's bottom edge */
    if (pending > 0) bar(ui, vec2_make(p.x + S(20), py + pill_h - S(7)), vec2_make(w - S(40), S(3)), sum->progress, a * 0.9f, sum->active > 0);
    core_clip_pop(r);
    ui_block(ui, p, sz);
    CORE_UNUSED(live);
}

/* ------------------------------------------------------------------------- */
/* page                                                                      */
/* ------------------------------------------------------------------------- */

static void draw_window_buttons(Ui *ui, Platform_Window *win, f32 right, f32 y, f32 a) {
    Core_Renderer *r = ui->r;
    const char *names[3] = { "dv.win.min", "dv.win.max", "dv.win.close" };
    f32 xs[3] = { right - S(110), right - S(69), right - S(29) };
    for (u32 i = 0; i < 3; i++) {
        vec2 c = vec2_make(xs[i], y);
        Ui_Interact it = ui_interact(ui, ui_id(names[i]), vec2_make(c.x - S(17), c.y - S(15)), vec2_make(S(34), S(30)), PLATFORM_CURSOR_DEFAULT);
        vec4 bg = i == 2 ? UI_RGBA(232, 72, 96, 0.9f * it.hover_t * a) : UI_RGBA(255, 255, 255, 0.08f * it.hover_t * a);
        if (it.hover_t > 0.01f) core_draw_rect_rounded(r, vec2_make(c.x - S(17), c.y - S(13)), vec2_make(S(34), S(26)), S(6), bg);
        vec4 col = ui_alpha(ui_mix(UI_RGBA(200, 196, 220, 1), UI_TEXT, it.hover_t), a);
        if (i == 0) ui_icon_minimize(ui, c, S(13), col);
        else if (i == 1) ui_icon_maximize(ui, c, S(12), col);
        else ui_icon_close(ui, c, S(13), col);
        if (it.clicked && win) {
            if (i == 0) platform_window_minimize(win);
            else if (i == 1) platform_window_toggle_maximize(win);
            else platform_window_request_close(win);
        }
    }
}

/* Keep the text tail of a long path within max_w: ".../Music/Downloads". */
static void path_tail(Ui *ui, const char *path, f32 px, f32 max_w, char *out, u32 cap) {
    Core_String full = core_str(path);
    if (ui_text_width(ui, ui->font, full, px) <= max_w) { copy_str(out, cap, path); return; }
    u64 at = 0;
    Core_String ell = core_str_lit("\xE2\x80\xA6");
    f32 room = max_w - ui_text_width(ui, ui->font, ell, px);
    while (at < full.len && ui_text_width(ui, ui->font, core_str_substr(full, at, full.len - at), px) > room) {
        at++;
        while (at < full.len && (full.str[at] & 0xC0) == 0x80) at++;
    }
    snprintf(out, cap, "\xE2\x80\xA6%s", path + at);
}

static void edit_query(Dl_View *v, const Platform_Input *in) {
    if (in->text_len) {
        u32 n = CORE_MIN(in->text_len, (u32)sizeof(v->query) - 1 - v->query_len);
        memcpy(v->query + v->query_len, in->text, n);
        v->query_len += n;
        v->query[v->query_len] = 0;
        v->caret_hold = 0.5f;
    }
    if (in->key_pressed[PLATFORM_KEY_BACKSPACE] && v->query_len) {
        if (in->ctrl) {
            while (v->query_len && v->query[v->query_len - 1] == ' ') v->query_len--;
            while (v->query_len && v->query[v->query_len - 1] != ' ') v->query_len--;
        } else {
            u32 n = v->query_len - 1;
            while (n > 0 && ((u8)v->query[n] & 0xC0) == 0x80) n--;
            v->query_len = n;
        }
        v->query[v->query_len] = 0;
        v->caret_hold = 0.5f;
    }
}

void dlv_draw(Dl_View *v, Ui *ui, const Dlv_Frame *f, Dlv_Out *out) {
    Core_Renderer *r = ui->r;
    const Platform_Input *in = ui->in;
    f32 open = ui_spring(ui, ui_id("dv.open"), v->open ? 1.0f : 0.0f, 360, 32);
    v->open_t = open;
    if (open < 0.004f && !v->open) return;
    ythumbs_update(v->thumbs);
    f32 vis = clamp01(open);
    b32 live = v->open && !f->blocked;
    vec2 win = ui->size;
    ui->input_enabled = live;

    Dl_Tools tools = downloads_tools(v->d);
    b32 tools_ok = tools.ytdlp && tools.ffmpeg;
    Dl_SearchInfo info = downloads_search_info(v->d);
    Dl_Summary sum = downloads_summary(v->d);

    /* someone else (the palette) started a search: adopt it */
    if (info.generation != v->gen) {
        v->gen = info.generation;
        reset_results(v);
        v->has_searched = info.state != DL_SEARCH_IDLE;
        copy_str(v->searched, sizeof(v->searched), info.query);
        v->searched_kind = info.kind;
    }
    if (info.count > v->seen_count) {
        for (u32 i = v->seen_count; i < info.count; i++) {
            v->seen_at[i] = ui->time + CORE_MIN((f64)(i - v->seen_count) * 0.03, 0.6);
            if (v->select_all_pending) v->checked[i] = 1;
        }
        v->seen_count = info.count;
    }

    /* ---- backdrop: the page covers the player ---- */
    core_draw_rect(r, vec2_zero(), win, UI_RGBA(11, 9, 21, 0.992f * vis));
    {
        /* slow drifting light pools so the page feels alive */
        core_set_blend(r, CORE_BLEND_ADD);
        b32 busy = info.state == DL_SEARCH_BUSY || sum.active > 0;
        if (busy) { v->drift += ui->dt * 0.12f; ui->animating = true; }
        f32 t = v->drift;
        core_draw_circle_ex(r, vec2_make(win.x * (0.22f + 0.06f * sinf(t)), win.y * (0.12f + 0.04f * cosf(t * 1.3f))), win.y * 0.55f, 0, win.y * 0.5f, UI_RGBA(110, 70, 255, 0.10f * vis));
        core_draw_circle_ex(r, vec2_make(win.x * (0.82f + 0.05f * cosf(t * 0.8f)), win.y * (0.86f + 0.05f * sinf(t))), win.y * 0.5f, 0, win.y * 0.45f, UI_RGBA(236, 72, 153, 0.05f * vis));
        core_set_blend(r, CORE_BLEND_NORMAL);
    }

    f32 slide = (1 - open) * S(28);
    f32 cw = CORE_MIN(S(880), win.x - S(56));
    f32 x0 = roundf((win.x - cw) * 0.5f);

    /* ---- keyboard ---- */
    b32 typing_ok = live && tools_ok;
    if (typing_ok) edit_query(v, in);
    if (live && in->key_pressed[PLATFORM_KEY_TAB]) {
        v->mode = v->mode == DL_QUERY_SONG ? DL_QUERY_ARTIST : DL_QUERY_SONG;
        if (v->query_len && tools_ok) start_search(v, false);
    }
    if (live && in->key_pressed[PLATFORM_KEY_ENTER] && tools_ok) {
        if (query_is_stale(v) || !info.count) {
            start_search(v, false);
        } else if (v->searched_kind == DL_QUERY_SONG) {
            if (enqueue_checked(v, f->lib, info.count, true)) v->dock_flash_until = ui->time + 2.6;
        } else {
            if (enqueue_checked(v, f->lib, info.count, false)) v->dock_flash_until = ui->time + 2.6;
        }
    }
    if (live && in->key_pressed[PLATFORM_KEY_ESCAPE]) {
        if (v->dock_pinned) v->dock_pinned = false;
        else dlv_open(v, false);
    }

    /* the dock is resolved first for input purposes: rows ignore clicks that land on it */
    b32 over_dock = v->dock_size.x > 0 && ui_mouse_in(ui, v->dock_pos, v->dock_size);

    /* ---- header ---- */
    f32 hy = S(46) - slide * 0.0f;
    {
        Ui_Interact back = round_button(ui, ui_id("dv.back"), vec2_make(x0 + S(14) - S(0), hy), S(19), vis);
        ui_icon_chevron_left(ui, vec2_make(x0 + S(14) - S(2) * back.hover_t, hy), S(24), ui_alpha(UI_TEXT, vis));
        ui_text(ui, ui->font_semi, core_str_lit("Get music"), x0 + S(46), hy, S(23), ui_alpha(UI_TEXT, vis), UI_ALIGN_LEFT, 0);
        if (back.clicked) dlv_open(v, false);

        /* where songs are saved */
        char tail[512];
        f32 chip_max = CORE_MIN(S(300), cw - S(300));
        path_tail(ui, f->dest_dir ? f->dest_dir : "", S(13), chip_max, tail, sizeof(tail));
        f32 tw = ui_text_width(ui, ui->font, core_str(tail), S(13));
        f32 chip_w = tw + S(46), chip_h = S(32);
        vec2 cp = vec2_make(win.x - S(132) - chip_w, hy - chip_h * 0.5f);
        Ui_Interact chip = ui_interact(ui, ui_id("dv.chip"), cp, vec2_make(chip_w, chip_h), PLATFORM_CURSOR_HAND);
        Core_BoxStyle cs = { .radius = chip_h * 0.5f, .fill = UI_RGBA(255, 255, 255, (0.04f + 0.05f * chip.hover_t) * vis),
                             .border = S(1), .border_color = UI_RGBA(255, 255, 255, (0.08f + 0.08f * chip.hover_t) * vis) };
        core_draw_box(r, cp, vec2_make(chip_w, chip_h), &cs);
        ui_icon_folder(ui, vec2_make(cp.x + S(18), hy), S(18), ui_alpha(UI_LAVENDER, vis));
        ui_text(ui, ui->font, core_str(tail), cp.x + S(34), hy, S(13), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, chip.hover_t), vis), UI_ALIGN_LEFT, 0);
        if (chip.clicked && out) out->open_settings = true;
        draw_window_buttons(ui, f->win, win.x, hy, vis);
    }

    /* ---- search bar ---- */
    /* centered until the first results are in, then docked under the header for good */
    if (!v->has_searched) v->bar_docked = false;
    else if (info.count || info.state != DL_SEARCH_BUSY) v->bar_docked = true;
    if (!tools_ok) v->bar_docked = true;
    f32 dock_e = ui_ease(ui, ui_id("dv.bar.dock"), v->bar_docked ? 1.0f : 0.0f, 9.0f);
    f32 top_y = S(96) + slide * 0.4f;
    f32 by = core_lerp(roundf((win.y - S(58)) * 0.5f), top_y, dock_e);
    vec2 bp = vec2_make(x0, by), bs = vec2_make(cw, S(58));
    {
        f32 en = tools_ok ? 1.0f : 0.45f;
        core_set_blend(r, CORE_BLEND_ADD);
        f32 glow = 0.6f + 0.4f * sinf((f32)ui->time * 2.4f);
        core_draw_shadow(r, bp, bs, S(14), S(14), UI_RGBA(139, 92, 246, (0.16f + 0.08f * glow) * vis * en));
        core_set_blend(r, CORE_BLEND_NORMAL);
        Core_BoxStyle ib = { .radius = S(14), .fill = UI_RGBA(20, 17, 34, 0.92f * vis),
                             .border = S(1.5f), .border_color = UI_RGBA(139, 102, 255, 0.95f * vis * en) };
        core_draw_box(r, bp, bs, &ib);

        /* mode switch with a sliding highlight */
        f32 seg_w = S(92), seg_h = S(40);
        f32 sx = bp.x + S(9), sy = bp.y + (bs.y - seg_h) * 0.5f;
        Core_BoxStyle sbg = { .radius = S(10), .fill = UI_RGBA(0, 0, 0, 0.3f * vis), .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.07f * vis) };
        core_draw_box(r, vec2_make(sx, sy), vec2_make(seg_w * 2 + S(6), seg_h), &sbg);
        f32 hx = ui_spring(ui, ui_id("dv.mode.hl"), sx + S(3) + seg_w * (f32)v->mode, 480, 34);
        Core_BoxStyle hl = { .radius = S(8), .fill = UI_RGBA(139, 102, 255, 0.30f * vis), .border = S(1), .border_color = UI_RGBA(160, 130, 255, 0.55f * vis) };
        core_draw_box(r, vec2_make(hx, sy + S(3)), vec2_make(seg_w, seg_h - S(6)), &hl);
        static const char *const names[2] = { "Song", "Artist" };
        for (u32 i = 0; i < 2; i++) {
            vec2 sp = vec2_make(sx + S(3) + seg_w * (f32)i, sy + S(3));
            Ui_Interact it = ui_interact(ui, ui_idx(ui_id("dv.mode"), i), sp, vec2_make(seg_w, seg_h - S(6)), PLATFORM_CURSOR_HAND);
            if (it.clicked && v->mode != i) {
                v->mode = i;
                if (v->query_len && tools_ok) start_search(v, false);
            }
            f32 act = ui_ease(ui, ui_idx(ui_id("dv.mode.a"), i), v->mode == i ? 1.0f : 0.0f, 14.0f);
            vec4 col = ui_alpha(ui_mix(ui_mix(UI_TEXT_DIM, UI_RGBA(210, 206, 228, 1), it.hover_t), UI_TEXT, act), vis);
            vec2 ic = vec2_make(sp.x + S(22), sp.y + (seg_h - S(6)) * 0.5f);
            if (i == 0) ui_icon_note(ui, ic, S(17), col); else ui_icon_person(ui, ic, S(17), col);
            ui_text(ui, ui->font_med, core_str(names[i]), sp.x + S(38), ic.y, S(14), col, UI_ALIGN_LEFT, 0);
        }
        f32 qx = sx + seg_w * 2 + S(6) + S(22);
        core_draw_rect(r, vec2_make(qx - S(10), bp.y + S(14)), vec2_make(S(1), bs.y - S(28)), UI_RGBA(255, 255, 255, 0.09f * vis));
        ui_icon_search(ui, vec2_make(qx + S(14), bp.y + bs.y * 0.5f), S(21), ui_alpha(UI_TEXT_DIM, vis));
        qx += S(40);
        f32 qy = bp.y + bs.y * 0.5f;
        f32 right_room = S(120);
        f32 qw = 0;
        if (v->query_len) {
            qw = ui_text(ui, ui->font_mono, (Core_String){ .str = (u8 *)v->query, .len = v->query_len }, qx, qy, S(18), ui_alpha(UI_TEXT, vis), UI_ALIGN_LEFT, bs.x - (qx - bp.x) - right_room);
        } else {
            const char *ph = v->mode == DL_QUERY_SONG ? "Song title, and the artist if you like" : "Artist or band name";
            ui_text(ui, ui->font, core_str(ph), qx + S(2), qy, S(16.5f), ui_alpha(UI_TEXT_FAINT, vis * en), UI_ALIGN_LEFT, bs.x - (qx - bp.x) - right_room);
        }
        if (tools_ok) {
            ui->animating = true; /* the caret blinks */
            f32 blink = fmodf((f32)(ui->time - v->opened_at), 1.06f) < 0.6f ? 1.0f : 0.25f;
            if (v->caret_hold > 0) { v->caret_hold -= ui->dt; blink = 1; }
            f32 cxp = ui_ease(ui, ui_id("dv.caret"), qx + qw + S(2), 30.0f);
            core_draw_rect(r, vec2_make(cxp, qy - S(11)), vec2_make(S(1.6f), S(22)), ui_alpha(UI_TEXT, vis * blink));
        }
        /* right end: spinner while searching, enter hint when there is something to run */
        f32 rx = bp.x + bs.x - S(26);
        if (info.state == DL_SEARCH_BUSY) {
            spinner(ui, vec2_make(rx, qy), S(9), S(2.2f), ui_alpha(UI_ACCENT_BRIGHT, vis));
        } else if (v->query_len && tools_ok && query_is_stale(v)) {
            Core_BoxStyle kb = { .radius = S(7), .fill = UI_RGBA(139, 102, 255, 0.22f * vis), .border = S(1), .border_color = UI_RGBA(160, 130, 255, 0.35f * vis) };
            Ui_Interact go = ui_interact(ui, ui_id("dv.go"), vec2_make(rx - S(20), qy - S(18)), vec2_make(S(40), S(36)), PLATFORM_CURSOR_HAND);
            core_draw_box(r, vec2_make(rx - S(20), qy - S(18)), vec2_make(S(40), S(36)), &kb);
            ui_icon_enter(ui, vec2_make(rx, qy), S(18), ui_alpha(ui_mix(UI_ACCENT_BRIGHT, UI_TEXT, go.hover_t), vis));
            if (go.clicked) start_search(v, false);
        } else if (v->query_len) {
            Ui_Interact cl = round_button(ui, ui_id("dv.clear"), vec2_make(rx, qy), S(15), vis);
            ui_icon_close(ui, vec2_make(rx, qy), S(14), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, cl.hover_t), vis));
            if (cl.clicked) { v->query_len = 0; v->query[0] = 0; }
        }
    }

    /* ---- body ---- */
    f32 bottom_pad = v->dock_size.x > 0 ? S(110) : S(24);
    vec2 body_pos = vec2_make(x0, top_y + bs.y + S(18) + slide * 0.2f);
    vec2 body_size = vec2_make(cw, win.y - body_pos.y - S(8));
    f32 body_a = vis * clamp01((dock_e - 0.55f) / 0.45f); /* fades in as the bar arrives */
    b32 rows_live = live && !over_dock && dock_e > 0.98f;

    if (!tools_ok) {
        if (draw_missing_tools(ui, body_pos, body_size, body_a, tools)) downloads_check_tools(v->d);
    } else if (!v->has_searched || body_a < 0.004f) {
        /* just the search bar */
    } else if (info.state == DL_SEARCH_FAILED && !info.count) {
        f32 cx = body_pos.x + body_size.x * 0.5f, cy = body_pos.y + CORE_MAX(S(70), body_size.y * 0.22f);
        Core_BoxStyle tb = { .radius = S(16), .fill = UI_RGBA(255, 255, 255, 0.06f * body_a), .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.1f * body_a) };
        core_draw_box(r, vec2_make(cx - S(28), cy - S(28)), vec2_make(S(56), S(56)), &tb);
        ui_icon_warning(ui, vec2_make(cx, cy), S(30), ui_alpha(UI_RGBA(255, 160, 182, 1), body_a));
        ui_text(ui, ui->font_semi, core_str_lit("No luck"), cx, cy + S(50), S(20), ui_alpha(UI_TEXT, body_a), UI_ALIGN_CENTER, 0);
        ui_text(ui, ui->font, core_str(info.error), cx, cy + S(80), S(14), ui_alpha(UI_TEXT_DIM, body_a), UI_ALIGN_CENTER, body_size.x - S(60));
        Core_String label = core_str_lit("Try again");
        f32 bw = ui_text_width(ui, ui->font_med, label, S(14)) + S(56), bh = S(38);
        if (pill_button(ui, ui_id("dv.retry"), label, vec2_make(cx - bw * 0.5f, cy + S(110)), vec2_make(bw, bh), true, true, body_a, ui_icon_retry).clicked)
            start_search(v, false);
    } else {
        if (draw_results(v, ui, f, &info, body_pos, body_size, body_a, rows_live, bottom_pad)) {
            if (enqueue_checked(v, f->lib, info.count, false)) {
                v->dock_flash_until = ui->time + 2.6;
                ui_burst(ui, vec2_make(body_pos.x + body_size.x - S(80), body_pos.y + S(23)), 16, UI_RGBA(190, 160, 255, 1), 0, S(130));
            }
        }
    }

    draw_dock(v, ui, win, vis, live, &sum);

    ui->input_enabled = true;
}

/* ------------------------------------------------------------------------- */
/* status pill (player screen)                                               */
/* ------------------------------------------------------------------------- */

b32 dlv_draw_status(Dl_View *v, Ui *ui, vec2 pos) {
    if (dlv_visible(v)) return false;
    Core_Renderer *r = ui->r;
    Dl_Summary sum = downloads_summary(v->d);
    u32 pending = sum.queued + sum.active;
    b32 busy = pending > 0;

    /* remember what just finished so the pill can say so for a few seconds */
    if (v->pill_was_busy && !busy) {
        v->pill_done_until = ui->time + 6.0;
        v->pill_done_count = sum.batch_total > sum.failed ? sum.batch_total - sum.failed : 0;
        v->pill_failed = sum.failed;
    }
    v->pill_was_busy = busy;
    b32 show_done = !busy && ui->time < v->pill_done_until;
    f32 shown = ui_spring(ui, ui_id("dv.pill.in"), (busy || show_done) ? 1.0f : 0.0f, 340, 28);
    if (shown < 0.01f && !busy && !show_done) return false;
    f32 a = clamp01(shown);
    if (show_done) ui->animating = true;

    char l1[96], l2[DL_TEXT * 3] = {0};
    if (busy) {
        u32 at = sum.batch_done + (sum.active ? 1 : 0);
        if (at > sum.batch_total) at = sum.batch_total;
        snprintf(l1, sizeof(l1), sum.paused ? "Paused  \xC2\xB7  %u left" : "Downloading  %u/%u", sum.paused ? pending : at, sum.batch_total);
        u32 n = downloads_briefs(v->d, v->briefs, MAX_JOBS_SHOWN);
        for (u32 i = 0; i < n; i++) {
            if (v->briefs[i].state != DL_ACTIVE) continue;
            Dl_Job j;
            if (downloads_job(v->d, i, &j) && j.uid == v->briefs[i].uid)
                snprintf(l2, sizeof(l2), "%s%s%s", j.artist, j.artist[0] ? " \xE2\x80\x94 " : "", j.title);
            break;
        }
    } else if (v->pill_failed) {
        snprintf(l1, sizeof(l1), "%u saved  \xC2\xB7  %u failed", v->pill_done_count, v->pill_failed);
    } else {
        snprintf(l1, sizeof(l1), v->pill_done_count == 1 ? "1 song saved" : "%u songs saved", v->pill_done_count);
    }

    f32 h = S(40);
    f32 w1 = ui_text_width(ui, ui->font_med, core_str(l1), S(14));
    f32 base_w = S(52) + w1 + S(18);
    f32 full_w = CORE_MAX(base_w, S(52) + CORE_MIN(ui_text_width(ui, ui->font, core_str(l2), S(12.5f)), S(260)) + S(18));
    vec2 hp = vec2_make(pos.x, pos.y - (1 - a) * S(10));
    vec2 sz = vec2_make(core_lerp(base_w, full_w, v->pill_expand), h + S(16) * v->pill_expand);
    Ui_Interact it = ui_interact(ui, ui_id("dv.pill"), hp, sz, PLATFORM_CURSOR_HAND);
    v->pill_expand = ui_ease(ui, ui_id("dv.pill.x"), it.hovered && l2[0] ? 1.0f : 0.0f, 12.0f);
    f32 w = core_lerp(base_w, full_w, v->pill_expand);
    sz = vec2_make(w, h + S(16) * v->pill_expand);
    core_draw_shadow(r, vec2_make(hp.x, hp.y + S(6)), sz, S(20), S(20), UI_RGBA(0, 0, 0, 0.4f * a));
    Core_BoxStyle bg = { .radius = S(20), .fill = UI_RGBA(26, 22, 43, (0.9f + 0.05f * it.hover_t) * a),
                         .border = S(1), .border_color = UI_RGBA(255, 255, 255, (0.10f + 0.08f * it.hover_t) * a) };
    core_draw_box(r, hp, sz, &bg);
    vec2 ic = vec2_make(hp.x + S(26), hp.y + h * 0.5f);
    if (busy) {
        ring(ui, ic, S(11), S(2.4f), sum.progress, UI_ACCENT_BRIGHT, a);
        ui_icon_download(ui, ic, S(11), ui_alpha(UI_TEXT, a));
        if (sum.active) ui->animating = true;
    } else if (v->pill_failed) {
        ui_icon_warning(ui, ic, S(18), UI_RGBA(255, 140, 170, a));
    } else {
        Core_BoxStyle ok = { .radius = S(11), .fill = UI_RGBA(128, 98, 236, 0.92f * a) };
        core_draw_box(r, vec2_make(ic.x - S(11), ic.y - S(11)), vec2_make(S(22), S(22)), &ok);
        ui_icon_check(ui, ic, S(13), 1, ui_alpha(UI_TEXT, a));
    }
    ui_text(ui, ui->font_med, core_str(l1), hp.x + S(46), hp.y + h * 0.5f - (v->pill_expand > 0.05f ? S(0) : 0), S(14), ui_alpha(UI_TEXT, a), UI_ALIGN_LEFT, 0);
    if (v->pill_expand > 0.02f && l2[0])
        ui_text(ui, ui->font, core_str(l2), hp.x + S(46), hp.y + h + S(2), S(12.5f), ui_alpha(UI_TEXT_DIM, a * v->pill_expand), UI_ALIGN_LEFT, w - S(60));
    return it.clicked;
}

/* ------------------------------------------------------------------------- */
/* API                                                                       */
/* ------------------------------------------------------------------------- */

Dl_View *dlv_create(Downloads *d, Core_Renderer *r, const char *cache_dir) {
    Dl_View *v = core_heap_calloc(sizeof(*v));
    v->d = d;
    v->thumbs = ythumbs_create(r, cache_dir);
    v->mode = DL_QUERY_SONG;
    return v;
}

void dlv_destroy(Dl_View *v) {
    ythumbs_destroy(v->thumbs);
    core_heap_free(v->lib_keys);
    core_heap_free(v);
}

void dlv_open(Dl_View *v, b32 open) {
    if (open == v->open) return;
    v->open = open;
    if (open) { v->caret_hold = 0.5f; v->opened_at = -1; }
}

b32 dlv_is_open(const Dl_View *v) { return v && v->open; }

b32 dlv_visible(const Dl_View *v) { return v && (v->open || v->open_t > 0.004f); }

void dlv_search(Dl_View *v, u32 kind, const char *query, b32 select_all) {
    v->mode = kind == DL_QUERY_ARTIST ? DL_QUERY_ARTIST : DL_QUERY_SONG;
    snprintf(v->query, sizeof(v->query), "%s", query);
    v->query_len = (u32)strlen(v->query);
    v->dock_pinned = false;
    start_search(v, select_all);
    dlv_open(v, true);
}

void dlv_start_empty(Dl_View *v, u32 kind) {
    v->mode = kind == DL_QUERY_ARTIST ? DL_QUERY_ARTIST : DL_QUERY_SONG;
    dlv_open(v, true);
}

void dlv_demo_download(Dl_View *v) {
    Dl_SearchInfo info = downloads_search_info(v->d);
    if (!info.count) return;
    if (v->searched_kind == DL_QUERY_SONG) {
        Dl_Result r;
        if (downloads_result(v->d, (u32)v->sel, &r)) downloads_enqueue(v->d, &r, 1, 0);
    } else {
        Dl_Result picks[DL_MAX_RESULTS];
        u32 n = 0;
        for (u32 i = 0; i < info.count; i++)
            if (v->checked[i] && downloads_result(v->d, i, &picks[n])) { n++; v->checked[i] = 0; }
        downloads_enqueue(v->d, picks, n, v->searched);
    }
    v->dock_flash_until = 1e18;
}

void dlv_demo_dock(Dl_View *v, b32 open) { v->dock_pinned = open; }
