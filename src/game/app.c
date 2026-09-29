/*
 * Offbeat application layer: state, playback queue, and every screen.
 *
 * Layout follows the design mock at 1376x972 "design pixels"; everything is
 * scaled by ui->scale for other window sizes (see S()). The left pane is the
 * now-playing stage (audio-reactive silk ribbon, cover card, progress ring,
 * transport); the right panel holds the Queue / Artists / Songs lists; the
 * Ctrl+K command palette floats above both.
 *
 * Lists are virtualized (only visible rows are touched) so library size is
 * unbounded. Nothing on the UI thread waits for disk: the library comes from
 * the index cache and is refreshed by a background scan, covers stream in
 * from worker threads, audio runs on its own thread.
 */

#include "app.h"

#include "covers.h"
#include "library.h"
#include "player.h"
#include "search.h"
#include "settings.h"
#include "ui.h"

#include "../core/font.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DESIGN_W 1376.0f
#define DESIGN_H 972.0f

#define SEARCH_MAX 64

enum { TAB_QUEUE = 0, TAB_GENRES, TAB_ARTISTS, TAB_SONGS, TAB_COUNT };
enum { REPEAT_OFF = 0, REPEAT_ALL, REPEAT_ONE };
enum { MENU_NONE = 0, MENU_TRACK, MENU_ARTIST, MENU_GENRE };

typedef struct {
    f32 target;   /* where the wheel wants the list          */
    f32 pos;      /* smoothed position drawn                 */
    f32 bar_t;    /* scrollbar visibility                    */
    f32 idle;     /* seconds since last scroll               */
    b32 dragging;
    f32 drag_off;
} Scroll;

struct App {
    Core_Arena *arena;
    Core_Arena  frame;         /* reset every frame */
    Core_Renderer *r;
    Platform_Window *win;
    Core_Text *text;
    Ui ui;

    const char *music_dir;
    const char *cache_dir;
    const char *config_dir;
    char index_path[1024];
    char state_path[1024];

    Library     *lib;
    Library     *lib_retired;  /* kept alive until the scanner is done with it */
    Lib_Scanner *scanner;
    f64          rescan_at;

    Covers  *covers;
    Player  *player;
    Spectrum spec;
    f32      vis_samples[SPECTRUM_FFT];
    f32      vis_time;

    Core_Effect *fx_background;

    /* queue (track indices into lib) */
    u32 *queue;
    u32 *queue_orig;           /* pre-shuffle order */
    u32  queue_len, queue_cap;
    s32  cur;                  /* index into queue, -1 = none */
    b32  shuffle;
    u32  repeat;
    u64  next_sent_hash;
    b32  next_sent;
    u64  seen_advance;
    u64  playing_hash;         /* track the engine is on */

    /* queue as last saved (path hashes), applied once the library is known */
    b32  session_pending;
    u64 *saved_q, *saved_o;
    u32  saved_q_len, saved_o_len;
    s32  saved_cur;
    u64  saved_track;
    f64  saved_pos;

    f32  volume;
    f32  volume_toast;         /* seconds of toast left */

    /* likes: open-addressing set of path hashes */
    u64 *likes;
    u32  likes_cap, likes_count;

    /* panel */
    u32    tab;
    s32    artist_open;        /* -1 = artist list */
    s32    genre_open;         /* -1 = genre list */
    Scroll scroll[TAB_COUNT + 2]; /* per tab, then the opened artist / genre */
    b32    queue_follow;       /* scroll queue to the current track */

    /* palette */
    b32  search_open;
    u8   query[256];
    u32  query_len;
    u32  query_hash_done;
    Search_Hit hits[SEARCH_MAX];
    u32  hit_count;
    s32  hit_sel;
    f32  hit_scroll;
    f64  search_opened_at;
    f32  caret_blink;

    /* context menu */
    u32  menu;
    u32  menu_track;
    s32  menu_queue_index;     /* >= 0 when opened on a queue row */
    b32  menu_artist_actions;  /* track menu also offers "queue artist" (palette) */
    u32  menu_artist;          /* Library.artists index for MENU_ARTIST */
    u32  menu_genre;           /* Library.genres index for MENU_GENRE */
    vec2 menu_pos;

    /* now playing text transition */
    u64  shown_hash;
    char shown_title[256], shown_artist[256];
    char prev_title[256], prev_artist[256];
    f32  swap_t;

    /* seek interaction */
    b32  seeking;
    f32  seek_preview;         /* 0..1 */
    f64  seek_hold_until;      /* keep preview until engine catches up */

    /* settings (see settings.h) and the modal that edits them */
    Settings settings;
    char     settings_path[1024];
    b32      settings_open;
    f32      theme_m[9];       /* color matrix, glides toward the chosen theme */

    /* folder browser page of the settings modal */
    b32          browse_open;
    char         browse_path[1024];
    Core_Arena   browse_arena; /* listing of browse_path, rebuilt on navigation */
    const char **browse_names;
    u32          browse_count;
    Scroll       browse_scroll;

    /* misc */
    f32  fps_smooth;
    f64  last_state_save;
    b32  state_dirty;
    f64  now;

    /* demo/screenshot script (OFFBEAT_DEMO) */
    const char *demo;
};

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */

static const char *str_c(App *app, Core_String s) {
    char *out = core_arena_push(&app->frame, s.len + 1, 1);
    memcpy(out, s.str, s.len);
    out[s.len] = 0;
    return out;
}

static Core_String str_fmt(App *app, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    u8 *p = core_arena_push(&app->frame, (u64)n + 1, 1);
    memcpy(p, buf, (size_t)n + 1);
    return (Core_String){ .str = p, .len = (u64)n };
}

static Core_String fmt_time(App *app, f64 s) {
    if (s < 0) s = 0;
    u32 t = (u32)s;
    if (t >= 3600) return str_fmt(app, "%u:%02u:%02u", t / 3600, (t / 60) % 60, t % 60);
    return str_fmt(app, "%u:%02u", t / 60, t % 60);
}

static void copy_str(char *dst, u32 cap, Core_String s) {
    u32 n = (u32)CORE_MIN(s.len, (u64)cap - 1);
    memcpy(dst, s.str, n);
    dst[n] = 0;
}

static f32 clamp01(f32 x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static f32 ease_out_cubic(f32 t) { t = clamp01(t); f32 u = 1 - t; return 1 - u * u * u; }

static const Lib_Track *track_at(App *app, u32 index) {
    if (!app->lib || index >= app->lib->track_count) return 0;
    return &app->lib->tracks[index];
}

static const Lib_Track *current_track(App *app) {
    if (app->cur < 0 || (u32)app->cur >= app->queue_len) return 0;
    return track_at(app, app->queue[app->cur]);
}

/* Deterministic pleasant color per hash (placeholders). */
static vec4 hash_color(u64 h, f32 a) {
    f32 hue = (f32)(h % 360) / 360.0f;
    f32 r = 0.5f + 0.5f * cosf(CORE_TAU * (hue + 0.0f));
    f32 g = 0.5f + 0.5f * cosf(CORE_TAU * (hue + 0.33f));
    f32 b = 0.5f + 0.5f * cosf(CORE_TAU * (hue + 0.67f));
    /* pull toward the violet theme and darken */
    return (vec4){ .x = 0.18f + r * 0.28f, .y = 0.12f + g * 0.2f, .z = 0.3f + b * 0.3f, .w = a };
}

/* ------------------------------------------------------------------------- */
/* likes                                                                     */
/* ------------------------------------------------------------------------- */

static void likes_grow(App *app) {
    u32 ncap = app->likes_cap ? app->likes_cap * 2 : 256;
    u64 *n = core_heap_calloc(sizeof(u64) * ncap);
    for (u32 i = 0; i < app->likes_cap; i++) {
        u64 h = app->likes[i];
        if (!h) continue;
        u32 j = (u32)h & (ncap - 1);
        while (n[j]) j = (j + 1) & (ncap - 1);
        n[j] = h;
    }
    core_heap_free(app->likes);
    app->likes = n;
    app->likes_cap = ncap;
}

static b32 is_liked(App *app, u64 h) {
    if (!app->likes_cap || !h) return false;
    u32 j = (u32)h & (app->likes_cap - 1);
    while (app->likes[j]) {
        if (app->likes[j] == h) return true;
        j = (j + 1) & (app->likes_cap - 1);
    }
    return false;
}

static void set_liked(App *app, u64 h, b32 on) {
    if (!h) return;
    if (on) {
        if (is_liked(app, h)) return;
        if ((app->likes_count + 1) * 2 > app->likes_cap) likes_grow(app);
        u32 j = (u32)h & (app->likes_cap - 1);
        while (app->likes[j]) j = (j + 1) & (app->likes_cap - 1);
        app->likes[j] = h;
        app->likes_count++;
    } else {
        if (!is_liked(app, h)) return;
        /* rebuild without h (rare operation) */
        u64 *old = app->likes;
        u32 cap = app->likes_cap;
        app->likes = core_heap_calloc(sizeof(u64) * cap);
        app->likes_count = 0;
        for (u32 i = 0; i < cap; i++) {
            if (!old[i] || old[i] == h) continue;
            u32 j = (u32)old[i] & (cap - 1);
            while (app->likes[j]) j = (j + 1) & (cap - 1);
            app->likes[j] = old[i];
            app->likes_count++;
        }
        core_heap_free(old);
    }
    app->state_dirty = true;
}

/* ------------------------------------------------------------------------- */
/* queue & playback                                                          */
/* ------------------------------------------------------------------------- */

static void queue_reserve(App *app, u32 n) {
    if (n <= app->queue_cap) return;
    u32 cap = app->queue_cap ? app->queue_cap : 256;
    while (cap < n) cap *= 2;
    app->queue = core_heap_realloc(app->queue, sizeof(u32) * cap);
    app->queue_orig = core_heap_realloc(app->queue_orig, sizeof(u32) * cap);
    app->queue_cap = cap;
}

static u32 rng_state = 0x9E3779B9u;
static u32 rng_next(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

/* Shuffle everything after the current item. */
static void queue_shuffle_upcoming(App *app) {
    if (app->queue_len < 3) return;
    u32 start = (u32)(app->cur + 1);
    for (u32 i = app->queue_len - 1; i > start; i--) {
        u32 j = start + rng_next() % (i - start + 1);
        u32 t = app->queue[i]; app->queue[i] = app->queue[j]; app->queue[j] = t;
    }
}

static void queue_set(App *app, const u32 *tracks, u32 count, u32 start) {
    queue_reserve(app, count);
    memcpy(app->queue, tracks, sizeof(u32) * count);
    memcpy(app->queue_orig, tracks, sizeof(u32) * count);
    app->queue_len = count;
    app->cur = count ? (s32)start : -1;
    app->state_dirty = true;
    if (app->shuffle && count) {
        /* current first, rest shuffled */
        u32 t = app->queue[0]; app->queue[0] = app->queue[start]; app->queue[start] = t;
        app->cur = 0;
        queue_shuffle_upcoming(app);
    }
    app->queue_follow = true;
}

static s32 next_index(App *app) {
    if (app->cur < 0 || app->queue_len == 0) return -1;
    if (app->repeat == REPEAT_ONE) return app->cur;
    s32 n = app->cur + 1;
    if ((u32)n >= app->queue_len) return app->repeat == REPEAT_ALL ? 0 : -1;
    return n;
}

static void sync_next(App *app) {
    s32 n = next_index(app);
    const Lib_Track *t = n >= 0 ? track_at(app, app->queue[n]) : 0;
    u64 h = t ? t->path_hash : 0;
    if (app->next_sent && app->next_sent_hash == h) return;
    player_set_next(app->player, t ? str_c(app, t->path) : 0, h);
    app->next_sent = true;
    app->next_sent_hash = h;
}

static void play_index(App *app, s32 qi, f64 start_s, b32 paused) {
    if (qi < 0 || (u32)qi >= app->queue_len) return;
    app->cur = qi;
    const Lib_Track *t = current_track(app);
    if (!t) return;
    player_play_file(app->player, str_c(app, t->path), t->path_hash, start_s, paused);
    app->playing_hash = t->path_hash;
    covers_set_art(app->covers, t);
    app->next_sent = false;
    sync_next(app);
    app->queue_follow = true;
    app->state_dirty = true;
}

static void play_tracks(App *app, const u32 *tracks, u32 count, u32 start) {
    queue_set(app, tracks, count, start);
    play_index(app, app->cur, 0, false);
}

static void toggle_pause(App *app) {
    Player_Status st = player_status(app->player);
    if (!st.loaded) {
        if (app->cur < 0 && app->queue_len) app->cur = 0;
        play_index(app, app->cur, 0, false);
        return;
    }
    if (st.ended) { play_index(app, app->cur, 0, false); return; }
    player_set_paused(app->player, !st.paused);
}

static void skip(App *app, s32 dir) {
    if (app->queue_len == 0) return;
    Player_Status st = player_status(app->player);
    if (dir < 0 && st.position_s > 3.0) { player_seek(app->player, 0); return; }
    s32 n = app->cur + dir;
    if (n < 0) n = app->repeat == REPEAT_ALL ? (s32)app->queue_len - 1 : 0;
    if ((u32)n >= app->queue_len) {
        if (app->repeat != REPEAT_ALL) return;
        n = 0;
    }
    play_index(app, n, 0, st.paused && st.loaded && !st.ended ? false : false);
}

static void set_shuffle(App *app, b32 on) {
    if (on == app->shuffle) return;
    app->shuffle = on;
    if (app->queue_len == 0) return;
    u32 cur_track = app->cur >= 0 ? app->queue[app->cur] : 0;
    if (on) {
        memcpy(app->queue_orig, app->queue, sizeof(u32) * app->queue_len);
        queue_shuffle_upcoming(app);
    } else {
        memcpy(app->queue, app->queue_orig, sizeof(u32) * app->queue_len);
        for (u32 i = 0; i < app->queue_len; i++)
            if (app->queue[i] == cur_track) { app->cur = (s32)i; break; }
    }
    app->next_sent = false;
    sync_next(app);
    app->state_dirty = true;
}

static void queue_insert_next_many(App *app, const u32 *tracks, u32 count) {
    if (!count) return;
    queue_reserve(app, app->queue_len + count);
    u32 at = app->cur >= 0 ? (u32)app->cur + 1 : 0;
    memmove(app->queue + at + count, app->queue + at, sizeof(u32) * (app->queue_len - at));
    memcpy(app->queue + at, tracks, sizeof(u32) * count);
    /* keep the unshuffled order roughly in sync: append */
    memcpy(app->queue_orig + app->queue_len, tracks, sizeof(u32) * count);
    app->queue_len += count;
    if (app->cur < 0) app->cur = 0;
    app->next_sent = false;
    sync_next(app);
    app->state_dirty = true;
}

static void queue_append_many(App *app, const u32 *tracks, u32 count) {
    if (!count) return;
    queue_reserve(app, app->queue_len + count);
    memcpy(app->queue + app->queue_len, tracks, sizeof(u32) * count);
    memcpy(app->queue_orig + app->queue_len, tracks, sizeof(u32) * count);
    app->queue_len += count;
    if (app->cur < 0) app->cur = 0;
    app->next_sent = false;
    sync_next(app);
    app->state_dirty = true;
}

static void queue_insert_next(App *app, u32 track) { queue_insert_next_many(app, &track, 1); }
static void queue_append(App *app, u32 track) { queue_append_many(app, &track, 1); }

static void queue_remove(App *app, u32 qi) {
    if (qi >= app->queue_len || (s32)qi == app->cur) return;
    u32 track = app->queue[qi];
    memmove(app->queue + qi, app->queue + qi + 1, sizeof(u32) * (app->queue_len - qi - 1));
    for (u32 i = 0; i < app->queue_len; i++) {
        if (app->queue_orig[i] == track) {
            memmove(app->queue_orig + i, app->queue_orig + i + 1, sizeof(u32) * (app->queue_len - i - 1));
            break;
        }
    }
    app->queue_len--;
    if ((s32)qi < app->cur) app->cur--;
    app->next_sent = false;
    sync_next(app);
    app->state_dirty = true;
}

/* Drop the whole queue, including the playing track, and stop playback. */
static void queue_clear(App *app) {
    if (app->queue_len == 0) return;
    app->queue_len = 0;
    app->cur = -1;
    player_stop(app->player);
    app->playing_hash = 0;
    app->scroll[TAB_QUEUE].target = 0;
    app->next_sent = false;
    sync_next(app);
    app->state_dirty = true;
}

/* ------------------------------------------------------------------------- */
/* library lifecycle                                                         */
/* ------------------------------------------------------------------------- */

static void restore_session(App *app);

/* Swap in a new snapshot, remapping the queue by path hash. */
static void adopt_library(App *app, Library *nl) {
    Library *old = app->lib;
    if (old && app->queue_len) {
        u32 w = 0;
        s32 new_cur = -1;
        for (u32 i = 0; i < app->queue_len; i++) {
            s64 ni = library_find(nl, old->tracks[app->queue[i]].path_hash);
            if (ni < 0) continue;
            if ((s32)i == app->cur) new_cur = (s32)w;
            app->queue[w++] = (u32)ni;
        }
        u32 wo = 0;
        for (u32 i = 0; i < app->queue_len; i++) {
            s64 ni = library_find(nl, old->tracks[app->queue_orig[i]].path_hash);
            if (ni >= 0) app->queue_orig[wo++] = (u32)ni;
        }
        app->queue_len = w;
        app->cur = new_cur >= 0 ? new_cur : (w ? 0 : -1);
    }
    app->lib = nl;
    if (app->artist_open >= (s32)nl->artist_count) app->artist_open = -1;
    if (app->genre_open >= (s32)nl->genre_count) app->genre_open = -1;
    if (old) {
        if (app->scanner && library_scanner_busy(app->scanner)) app->lib_retired = old;
        else library_free(old);
    }
    if (app->session_pending) restore_session(app);
    else if (app->queue_len == 0 && nl->track_count) {
        queue_set(app, nl->by_title, nl->track_count, 0);
    }
    /* the loaded file left the library (e.g. the music folder changed):
       cue the new current track instead of keeping a ghost loaded */
    if (app->playing_hash && library_find(nl, app->playing_hash) < 0) {
        player_stop(app->player);
        app->playing_hash = 0;
        if (app->cur >= 0) play_index(app, app->cur, 0, true);
    }
    app->next_sent = false;
    app->query_hash_done = 0; /* re-run search */
    covers_prefetch_library(app->covers, nl);
}

static void poll_scanner(App *app) {
    if (!app->scanner) {
        if (app->rescan_at > 0 && app->now >= app->rescan_at) {
            app->rescan_at = 0;
            app->scanner = library_scan_start(app->music_dir, app->index_path, app->lib);
        }
        return;
    }
    b32 changed = false;
    Library *nl = library_scanner_poll(app->scanner, &changed);
    if (nl) {
        if (changed || !app->lib) adopt_library(app, nl);
        else library_free(nl);
    }
    if (!library_scanner_busy(app->scanner)) {
        library_scanner_destroy(app->scanner);
        app->scanner = 0;
        if (app->lib_retired) { library_free(app->lib_retired); app->lib_retired = 0; }
    }
}

/* ------------------------------------------------------------------------- */
/* persisted state                                                           */
/* ------------------------------------------------------------------------- */

static void save_state(App *app) {
    Core_Temp tmp = core_temp_begin(&app->frame);
    u64 cap = 4096 + (u64)app->likes_count * 24 + (u64)app->queue_len * 2 * 24;
    char *buf = core_arena_push(&app->frame, cap, 1);
    u64 n = 0;
    Player_Status st = player_status(app->player);
    const Lib_Track *t = current_track(app);
    n += (u64)snprintf(buf + n, cap - n, "offbeat-state 1\nvolume %.3f\nshuffle %d\nrepeat %u\ntab %u\n",
                       app->volume, app->shuffle ? 1 : 0, app->repeat, app->tab);
    if (t) n += (u64)snprintf(buf + n, cap - n, "track %016llx %.2f\n",
                              (unsigned long long)t->path_hash, st.loaded ? st.position_s : 0.0);
    for (u32 i = 0; i < app->likes_cap && n + 32 < cap; i++)
        if (app->likes[i]) n += (u64)snprintf(buf + n, cap - n, "like %016llx\n", (unsigned long long)app->likes[i]);
    if (app->lib && app->queue_len) {
        n += (u64)snprintf(buf + n, cap - n, "cur %d\n", app->cur);
        for (u32 i = 0; i < app->queue_len && n + 32 < cap; i++)
            n += (u64)snprintf(buf + n, cap - n, "q %016llx\n", (unsigned long long)track_at(app, app->queue[i])->path_hash);
        if (app->shuffle) /* pre-shuffle order, so un-shuffling still works after a restart */
            for (u32 i = 0; i < app->queue_len && n + 32 < cap; i++)
                n += (u64)snprintf(buf + n, cap - n, "o %016llx\n", (unsigned long long)track_at(app, app->queue_orig[i])->path_hash);
    }
    platform_make_dirs(app->config_dir);
    platform_file_write_all(app->state_path, buf, n);
    core_temp_end(tmp);
    app->state_dirty = false;
    app->last_state_save = app->now;
}

static void load_state(App *app) {
    Core_Temp tmp = core_temp_begin(&app->frame);
    Core_String s = platform_file_read_all(&app->frame, app->state_path);
    if (s.len) {
        u32 lines = 0;
        for (u64 i = 0; i < s.len; i++) lines += s.str[i] == '\n';
        app->saved_q = core_heap_calloc(sizeof(u64) * (lines + 1));
        app->saved_o = core_heap_calloc(sizeof(u64) * (lines + 1));
        char *line = (char *)s.str;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            unsigned long long h;
            double d;
            unsigned u;
            int cur;
            if (line[0] == 'q' && line[1] == ' ') app->saved_q[app->saved_q_len++] = strtoull(line + 2, 0, 16);
            else if (line[0] == 'o' && line[1] == ' ') app->saved_o[app->saved_o_len++] = strtoull(line + 2, 0, 16);
            else if (sscanf(line, "cur %d", &cur) == 1) app->saved_cur = cur;
            else if (sscanf(line, "volume %lf", &d) == 1) app->volume = (f32)CORE_CLAMP(d, 0.0, 1.0);
            else if (sscanf(line, "shuffle %u", &u) == 1) app->shuffle = u != 0;
            else if (sscanf(line, "repeat %u", &u) == 1) app->repeat = u % 3;
            else if (sscanf(line, "tab %u", &u) == 1) app->tab = u % TAB_COUNT;
            else if (sscanf(line, "track %llx %lf", &h, &d) == 2) { app->saved_track = h; app->saved_pos = d; }
            else if (sscanf(line, "like %llx", &h) == 1) set_liked(app, h, true);
            line = nl ? nl + 1 : 0;
        }
    }
    core_temp_end(tmp);
    app->state_dirty = false;
    app->session_pending = app->saved_track || app->saved_q_len;
}

/* Settings are written as soon as they change (they change rarely, and only
   from a click), unlike the state file which is batched. */
static void save_settings(App *app) {
    char buf[2048];
    u64 n = settings_format(&app->settings, buf, sizeof(buf));
    platform_make_dirs(app->config_dir);
    platform_file_write_all(app->settings_path, buf, n);
}

static void load_settings(App *app) {
    Core_Temp tmp = core_temp_begin(&app->frame);
    settings_default(&app->settings);
    settings_parse(&app->settings, platform_file_read_all(&app->frame, app->settings_path));
    core_temp_end(tmp);
}

/* Start a fresh scan now, abandoning one in flight. */
static void rescan_library(App *app) {
    if (app->scanner) {
        library_scanner_destroy(app->scanner); /* cancels, then joins */
        app->scanner = 0;
        if (app->lib_retired) { library_free(app->lib_retired); app->lib_retired = 0; }
    }
    app->rescan_at = 0;
    app->scanner = library_scan_start(app->music_dir, app->index_path, app->lib);
}

/* Switch the library root. The rescan reuses unchanged tracks and drops the
   ones outside the new folder; the queue is remapped when it lands. */
static void set_music_dir(App *app, const char *dir) {
    if (strcmp(dir, app->music_dir) == 0) return;
    snprintf(app->settings.music_dir, sizeof(app->settings.music_dir), "%s", dir);
    app->music_dir = app->settings.music_dir;
    save_settings(app);
    rescan_library(app);
}

/* Rebuild the queue and the paused now-playing track from the saved state.
   Falls back to the whole library (by title) when no queue was saved. */
static void restore_session(App *app) {
    const Library *lib = app->lib;
    app->session_pending = false;
    if (!lib || !lib->track_count) return;

    b32 shuffle = app->shuffle;
    s32 cur = -1;
    if (app->saved_q_len) {
        queue_reserve(app, app->saved_q_len);
        u32 w = 0;
        for (u32 i = 0; i < app->saved_q_len; i++) {
            s64 ti = library_find(lib, app->saved_q[i]);
            if (ti < 0) continue;
            if ((s32)i == app->saved_cur) cur = (s32)w;
            app->queue[w++] = (u32)ti;
        }
        app->queue_len = w;
        u32 wo = 0;
        if (shuffle && app->saved_o_len) {
            for (u32 i = 0; i < app->saved_o_len && wo < w; i++) {
                s64 ti = library_find(lib, app->saved_o[i]);
                if (ti >= 0) app->queue_orig[wo++] = (u32)ti;
            }
        }
        if (wo != w) memcpy(app->queue_orig, app->queue, sizeof(u32) * w);
        app->queue_follow = true;
    }
    if (app->queue_len == 0) {
        app->shuffle = false;
        queue_set(app, lib->by_title, lib->track_count, 0);
        app->shuffle = shuffle;
        s64 ti = app->saved_track ? library_find(lib, app->saved_track) : -1;
        if (ti >= 0) {
            for (u32 i = 0; i < app->queue_len; i++)
                if (app->queue[i] == (u32)ti) { cur = (s32)i; break; }
        }
        if (shuffle) set_shuffle(app, true);
        if (cur < 0) cur = app->cur;
    }
    if (cur < 0) cur = 0;
    app->cur = cur;

    const Lib_Track *t = current_track(app);
    f64 pos = t && t->path_hash == app->saved_track ? app->saved_pos : 0;
    play_index(app, cur, pos, true);
    covers_prefetch_library(app->covers, lib);

    core_heap_free(app->saved_q); core_heap_free(app->saved_o);
    app->saved_q = app->saved_o = 0;
    app->saved_q_len = app->saved_o_len = 0;
}

/* ------------------------------------------------------------------------- */
/* init                                                                      */
/* ------------------------------------------------------------------------- */

static const char *path_join(Core_Arena *a, const char *dir, const char *name) {
    u64 n = strlen(dir) + strlen(name) + 2;
    char *p = core_arena_push(a, n, 1);
    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

static const char *find_asset(App *app, const char *rel) {
    const char *exe = platform_exe_dir(app->arena);
    const char *cands[] = { "../assets", "assets", "../share/offbeat/assets", "../../assets" };
    for (u32 i = 0; i < CORE_ARRAY_COUNT(cands); i++) {
        const char *dir = path_join(app->arena, exe, cands[i]);
        const char *p = path_join(app->arena, dir, rel);
        if (platform_file_info(p).exists) return p;
    }
    return path_join(app->arena, "assets", rel);
}

static const char *BACKGROUND_FX;

App *app_create(Core_Arena *arena, Core_Renderer *r, Platform_Window *win) {
    App *app = core_push_struct(arena, App);
    app->arena = arena;
    app->r = r;
    app->win = win;
    core_arena_init(&app->frame, CORE_GB(1));
    app->volume = 0.8f;
    app->cur = -1;
    app->artist_open = -1;
    app->genre_open = -1;
    app->repeat = REPEAT_OFF;
    app->demo = platform_env("OFFBEAT_DEMO");

    /* ---- fonts ---- */
    app->text = core_text_create(arena, r, platform_file_map);
    const char *fallbacks[] = {
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    };
    const char *chain[4];
    chain[1] = fallbacks[0]; chain[2] = fallbacks[1]; chain[3] = fallbacks[2];

    ui_init(&app->ui, arena, r, app->text);
    chain[0] = find_asset(app, "fonts/Outfit-Regular.ttf");
    app->ui.font = core_text_font(app->text, chain, 4);
    chain[0] = find_asset(app, "fonts/Outfit-Medium.ttf");
    app->ui.font_med = core_text_font(app->text, chain, 4);
    chain[0] = find_asset(app, "fonts/Outfit-SemiBold.ttf");
    app->ui.font_semi = core_text_font(app->text, chain, 4);
    chain[0] = find_asset(app, "fonts/JetBrainsMono-Regular.ttf");
    app->ui.font_mono = core_text_font(app->text, chain, 4);
    if (!core_text_font_ok(app->text, app->ui.font_med))  app->ui.font_med = app->ui.font;
    if (!core_text_font_ok(app->text, app->ui.font_semi)) app->ui.font_semi = app->ui.font_med;
    if (!core_text_font_ok(app->text, app->ui.font_mono)) app->ui.font_mono = app->ui.font;

    app->fx_background = core_effect_create(r, "background", BACKGROUND_FX);

    /* ---- paths & settings ---- */
    app->cache_dir  = platform_env("OFFBEAT_CACHE_DIR") ? platform_env("OFFBEAT_CACHE_DIR") : platform_cache_dir(arena);
    app->config_dir = platform_env("OFFBEAT_CONFIG_DIR") ? platform_env("OFFBEAT_CONFIG_DIR") : platform_config_dir(arena);
    platform_make_dirs(app->cache_dir);
    snprintf(app->index_path, sizeof(app->index_path), "%s/library.idx", app->cache_dir);
    snprintf(app->state_path, sizeof(app->state_path), "%s/state", app->config_dir);
    snprintf(app->settings_path, sizeof(app->settings_path), "%s/settings", app->config_dir);
    load_settings(app);
    settings_theme_matrix(app->settings.theme, app->theme_m);
    const char *music = platform_env("OFFBEAT_MUSIC_DIR");
    app->music_dir = music ? music : app->settings.music_dir[0] ? app->settings.music_dir : platform_music_dir(arena);
    core_arena_init(&app->browse_arena, CORE_MB(64));

    /* ---- subsystems ---- */
    app->player = player_create();
    app->covers = covers_create(r, app->cache_dir, CORE_CLAMP(platform_cpu_count() / 4, 2u, 3u));

    load_state(app);
    player_set_volume(app->player, app->volume);

    /* Library: instant from cache, then refresh in the background. */
    app->lib = library_load_cache(app->index_path);
    restore_session(app);

    /* Scan right away when there's no cache; otherwise shortly after startup
       so the first frames get the whole machine. */
    b32 same_root = app->lib && core_str_eq(app->lib->music_dir, core_str(app->music_dir));
    app->rescan_at = same_root ? 1.5 : 0.001;
    return app;
}

void app_shutdown(App *app) {
    save_state(app);
    if (app->scanner) library_scanner_destroy(app->scanner);
    player_destroy(app->player);
    covers_destroy(app->covers);
}

/* ------------------------------------------------------------------------- */
/* shaders                                                                   */
/* ------------------------------------------------------------------------- */

/* Background: deep indigo base with slow drifting light pools tinted by the
   current artwork's palette, plus an extremely blurred version of the art
   itself (high mip) for color continuity. Dithered to avoid banding. */
static const char *BACKGROUND_FX =
    "float hash(vec2 p){ return fract(sin(dot(p, vec2(12.9898,78.233))) * 43758.5453); }\n"
    "float blob(vec2 uv, vec2 c, float r){ float d = length(uv - c) / r; return exp(-d*d*2.2); }\n"
    "void main(){\n"
    "  vec2 res = u_p[0].xy;\n"
    "  vec2 uv = v_px / res;\n"
    "  float asp = res.x / res.y;\n"
    "  vec2 q = vec2(uv.x * asp, uv.y);\n"
    "  float t = u_time * 0.06;\n"
    "  vec3 c_dark = u_p[1].rgb, c_mid = u_p[2].rgb, c_warm = u_p[3].rgb, c_vivid = u_p[4].rgb;\n"
    "  float energy = u_p[5].x, beat = u_p[5].y, art = u_p[5].z;\n"
    "  vec3 col = u_p[6].rgb;\n"
    "  col = mix(col, c_dark * 0.35 + vec3(0.03,0.025,0.06), 0.5);\n"
    "  vec2 p1 = vec2(0.10*asp + 0.06*sin(t*1.3), 0.10 + 0.06*cos(t*1.1));\n"
    "  vec2 p2 = vec2(0.42*asp + 0.10*sin(t*0.7+1.0), 1.05 + 0.04*cos(t*0.9));\n"
    "  vec2 p3 = vec2(0.62*asp + 0.08*cos(t*0.8+2.0), 0.28 + 0.07*sin(t*1.2));\n"
    "  vec2 p4 = vec2(0.02*asp + 0.05*cos(t*0.6+4.0), 0.85 + 0.05*sin(t*0.7));\n"
    "  float pulse = 1.0 + 0.25*energy + 0.25*beat;\n"
    "  col += c_warm  * blob(q, p1, 0.45) * 0.42 * pulse;\n"
    "  col += c_warm  * blob(q, p2, 0.52) * 0.38 * pulse;\n"
    "  col += c_vivid * blob(q, p3, 0.55) * 0.12 * pulse;\n"
    "  col += c_mid   * blob(q, p4, 0.45) * 0.16;\n"
    "  vec2 wuv = uv + 0.04*vec2(sin(uv.y*3.0 + t*2.0), cos(uv.x*3.0 - t*1.7));\n"
    "  vec3 blur = textureLod(u_tex0, clamp(wuv, 0.0, 1.0), 6.0).rgb;\n"
    "  col += blur * blur * 0.22 * art;\n"
    "  float vig = smoothstep(1.25, 0.2, length((uv - vec2(0.4, 0.45)) * vec2(1.1, 1.0)));\n"
    "  col *= mix(0.55, 1.0, vig);\n"
    "  col += (hash(v_px + fract(u_time)) - 0.5) / 255.0;\n"
    "  frag = vec4(col, 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------------- */
/* stage: background, ribbon, ring, cover                                    */
/* ------------------------------------------------------------------------- */

static vec3 v3(f32 r, f32 g, f32 b) { return vec3_make(r, g, b); }

/* Palette for the scene: from the artwork when available, else defaults
   matching the design (warm orange/pink + violet). Crossfades on change. */
typedef struct { vec3 dark, mid, warm, vivid; f32 art; } Scene_Colors;

static Scene_Colors scene_colors(App *app) {
    Ui *ui = &app->ui;
    Cover_Art art = covers_art(app->covers);
    Scene_Colors d = {
        .dark = v3(0.12f, 0.08f, 0.22f), .mid = v3(0.42f, 0.26f, 0.72f),
        .warm = v3(0.95f, 0.45f, 0.36f), .vivid = v3(0.62f, 0.36f, 1.0f), .art = 0,
    };
    /* the design's defaults follow the theme (except the warm counter-light,
       which rotated would turn green); artwork colors stay true */
    d.dark  = settings_theme_apply(app->theme_m, d.dark);
    d.mid   = settings_theme_apply(app->theme_m, d.mid);
    d.vivid = settings_theme_apply(app->theme_m, d.vivid);
    Scene_Colors target = d;
    if (art.ready && !art.missing) {
        target.dark  = art.palette[0];
        target.mid   = art.palette[1];
        /* keep the warm glow in the design's family but tinted by the art */
        target.warm  = vec3_lerp(d.warm, art.palette[2], 0.55f);
        target.vivid = vec3_lerp(d.vivid, art.palette[3], 0.6f);
        target.art = 1;
    }
    Scene_Colors c;
    f32 rate = 3.0f;
    c.dark.x  = ui_ease(ui, ui_id("sc.dark.r"), target.dark.x, rate);
    c.dark.y  = ui_ease(ui, ui_id("sc.dark.g"), target.dark.y, rate);
    c.dark.z  = ui_ease(ui, ui_id("sc.dark.b"), target.dark.z, rate);
    c.mid.x   = ui_ease(ui, ui_id("sc.mid.r"), target.mid.x, rate);
    c.mid.y   = ui_ease(ui, ui_id("sc.mid.g"), target.mid.y, rate);
    c.mid.z   = ui_ease(ui, ui_id("sc.mid.b"), target.mid.z, rate);
    c.warm.x  = ui_ease(ui, ui_id("sc.warm.r"), target.warm.x, rate);
    c.warm.y  = ui_ease(ui, ui_id("sc.warm.g"), target.warm.y, rate);
    c.warm.z  = ui_ease(ui, ui_id("sc.warm.b"), target.warm.z, rate);
    c.vivid.x = ui_ease(ui, ui_id("sc.viv.r"), target.vivid.x, rate);
    c.vivid.y = ui_ease(ui, ui_id("sc.viv.g"), target.vivid.y, rate);
    c.vivid.z = ui_ease(ui, ui_id("sc.viv.b"), target.vivid.z, rate);
    c.art     = ui_ease(ui, ui_id("sc.art"), target.art, rate);
    return c;
}

static void draw_background(App *app, vec2 size, Scene_Colors sc) {
    Cover_Art art = covers_art(app->covers);
    vec4 p[7];
    p[0] = (vec4){ .x = size.x, .y = size.y };
    p[1] = ui_rgb(sc.dark, 1);
    p[2] = ui_rgb(sc.mid, 1);
    p[3] = ui_rgb(sc.warm, 1);
    p[4] = ui_rgb(sc.vivid, 1);
    p[5] = (vec4){ .x = app->spec.level, .y = app->spec.beat, .z = art.ready ? sc.art : 0 };
    p[6] = ui_rgb(settings_theme_apply(app->theme_m, v3(0.050f, 0.043f, 0.090f)), 1); /* base */
    core_draw_effect(app->r, app->fx_background, vec2_zero(), size, (f32)app->now,
                     p, 7, art.tex, (Core_Texture){0});
}

/* Catmull-Rom through the ribbon's control points (in units of R). */
static const f32 RIBBON_PTS[][2] = {
    { -1.50f,  0.20f }, { -1.18f,  0.16f }, { -0.82f,  0.02f }, { -0.45f, -0.30f },
    { -0.05f, -0.62f }, {  0.30f, -0.68f }, {  0.55f, -0.36f }, {  0.64f,  0.18f },
    {  0.56f,  0.72f }, {  0.44f,  1.05f },
};
#define RIBBON_N ((s32)CORE_ARRAY_COUNT(RIBBON_PTS))

static vec2 ribbon_point(f32 s) {
    f32 f = s * (f32)(RIBBON_N - 1);
    s32 i = (s32)f;
    if (i > RIBBON_N - 2) i = RIBBON_N - 2;
    f32 t = f - (f32)i;
    s32 i0 = i > 0 ? i - 1 : 0, i1 = i, i2 = i + 1, i3 = i + 2 < RIBBON_N ? i + 2 : RIBBON_N - 1;
    f32 t2 = t * t, t3 = t2 * t;
    f32 x = 0.5f * ((2 * RIBBON_PTS[i1][0]) + (-RIBBON_PTS[i0][0] + RIBBON_PTS[i2][0]) * t +
                    (2 * RIBBON_PTS[i0][0] - 5 * RIBBON_PTS[i1][0] + 4 * RIBBON_PTS[i2][0] - RIBBON_PTS[i3][0]) * t2 +
                    (-RIBBON_PTS[i0][0] + 3 * RIBBON_PTS[i1][0] - 3 * RIBBON_PTS[i2][0] + RIBBON_PTS[i3][0]) * t3);
    f32 y = 0.5f * ((2 * RIBBON_PTS[i1][1]) + (-RIBBON_PTS[i0][1] + RIBBON_PTS[i2][1]) * t +
                    (2 * RIBBON_PTS[i0][1] - 5 * RIBBON_PTS[i1][1] + 4 * RIBBON_PTS[i2][1] - RIBBON_PTS[i3][1]) * t2 +
                    (-RIBBON_PTS[i0][1] + 3 * RIBBON_PTS[i1][1] - 3 * RIBBON_PTS[i2][1] + RIBBON_PTS[i3][1]) * t3);
    return vec2_make(x, y);
}

static vec3 ribbon_color(f32 s, Scene_Colors sc) {
    /* hot pink -> magenta -> violet -> electric blue along the ribbon */
    vec3 pink = vec3_lerp(v3(1.0f, 0.28f, 0.42f), sc.warm, 0.2f);
    vec3 mag  = v3(0.92f, 0.28f, 0.86f);
    vec3 vio  = vec3_lerp(v3(0.62f, 0.38f, 1.0f), sc.vivid, 0.25f);
    vec3 blue = v3(0.34f, 0.52f, 1.0f);
    if (s < 0.30f) return vec3_lerp(pink, mag, s / 0.30f);
    if (s < 0.55f) return vec3_lerp(mag, vio, (s - 0.30f) / 0.25f);
    return vec3_lerp(vio, blue, clamp01((s - 0.55f) / 0.3f));
}

static f32 hash01(u32 x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return (f32)(x & 0xFFFFFF) / (f32)0x1000000;
}

/* The silk ribbon: ~44 strands x 64 segments of additive hairlines displaced
   by the spectrum. All CPU-side geometry through the instanced renderer. */
static void draw_ribbon(App *app, vec2 C, f32 R, Scene_Colors sc, f32 alpha) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    Spectrum *sp = &app->spec;
    f32 t = app->vis_time;
    enum { STRANDS = 60, SEGS = 84 };

    /* Centerline frame (point, normal, envelope) shared by every strand. */
    vec2 base[SEGS + 1], nrm[SEGS + 1];
    f32 env[SEGS + 1];
    for (u32 j = 0; j <= SEGS; j++) {
        f32 s = (f32)j / (f32)SEGS;
        base[j] = ribbon_point(s);
        vec2 pn = ribbon_point(CORE_MIN(s + 0.01f, 1.0f));
        vec2 pm = ribbon_point(CORE_MAX(s - 0.01f, 0.0f));
        nrm[j] = vec2_perpendicular(vec2_normalize_safe(vec2_sub(pn, pm)));
        env[j] = powf(sinf(CORE_PI * powf(s, 0.8f)), 1.2f);
    }

    core_set_blend(r, CORE_BLEND_ADD);

    /* soft luminous body underneath the strands */
    for (u32 j = 0; j < SEGS; j += 2) {
        f32 s0 = (f32)j / SEGS;
        vec2 a = vec2_add(C, vec2_mul(base[j], R));
        vec2 b = vec2_add(C, vec2_mul(base[j + 2 <= SEGS ? j + 2 : SEGS], R));
        f32 w = R * (0.06f + 0.34f * env[j]) * (1.0f + 0.25f * sp->bass);
        f32 ga = (0.030f + 0.03f * sp->level + 0.04f * sp->beat) * alpha;
        core_draw_line_ex(r, a, b, w * 0.25f, w, ui_rgb(ribbon_color(s0, sc), ga),
                          ui_rgb(ribbon_color(s0 + 2.0f / SEGS, sc), ga));
    }

    f32 th = CORE_MAX(1.0f, S(1.15f));
    for (u32 i = 0; i < STRANDS; i++) {
        f32 u = (f32)i / (f32)(STRANDS - 1) - 0.5f;
        f32 band = sp->bands[(i * SPECTRUM_BANDS) / STRANDS];
        f32 edge = fabsf(u) * 2.0f;
        /* silk: bright folded edges, translucent interior */
        f32 base_a = (0.085f + 0.40f * edge * edge * edge) * alpha;
        f32 whiten = 0.35f * edge * edge * edge * edge;
        vec2 prev = {0};
        for (u32 j = 0; j <= SEGS; j++) {
            f32 s = (f32)j / (f32)SEGS;
            f32 e = env[j];
            f32 width = (0.05f + 0.70f * e) * (1.0f + 0.16f * sp->level + 0.1f * sp->beat);
            f32 wob = sinf(s * 7.0f + t * 1.1f + u * 3.0f) * 0.07f
                    + sinf(s * 15.0f - t * 1.7f + u * 7.0f) * (0.012f + 0.07f * band)
                    + sinf(s * 4.0f + t * 0.5f) * 0.05f * (1.0f + sp->bass);
            f32 twist = sinf(s * 4.5f + t * 0.35f + u * 0.8f) * 0.42f + 0.58f; /* ribbon folds */
            f32 off = u * width * twist + wob * e;
            vec2 pt = vec2_add(C, vec2_mul(vec2_add(base[j], vec2_mul(nrm[j], off)), R));
            if (j > 0) {
                f32 fade = clamp01(s * 7.0f) * clamp01((1.0f - s) * 4.0f);
                f32 a = base_a * fade * (0.8f + 0.7f * band + 0.4f * sp->beat);
                vec3 c = vec3_lerp(ribbon_color(s, sc), v3(1, 0.92f, 1), whiten);
                core_draw_line(r, prev, pt, th, ui_rgb(c, a));
            }
            prev = pt;
        }
    }

    /* faint cross ribs give the surface its woven look */
    for (u32 j = 4; j < SEGS - 2; j += 3) {
        f32 s = (f32)j / (f32)SEGS;
        f32 width = (0.05f + 0.70f * env[j]);
        f32 twist = sinf(s * 4.5f + t * 0.35f) * 0.42f + 0.58f;
        vec2 a = vec2_add(C, vec2_mul(vec2_add(base[j], vec2_mul(nrm[j], -0.5f * width * twist)), R));
        vec2 b = vec2_add(C, vec2_mul(vec2_add(base[j], vec2_mul(nrm[j], 0.5f * width * twist)), R));
        vec3 c = ribbon_color(s, sc);
        core_draw_line_ex(r, a, b, th * 0.8f, 0, ui_rgb(c, 0.05f * alpha), ui_rgb(c, 0.05f * alpha));
    }

    /* stardust */
    for (u32 i = 0; i < 110; i++) {
        f32 s = hash01(i * 3 + 1);
        f32 u = hash01(i * 3 + 2) - 0.5f;
        f32 drift = t * (0.004f + 0.01f * hash01(i * 7 + 5));
        s = fmodf(s + drift, 1.0f);
        vec2 p = ribbon_point(s);
        vec2 pn = ribbon_point(CORE_MIN(s + 0.01f, 1.0f));
        vec2 n = vec2_perpendicular(vec2_normalize_safe(vec2_sub(pn, p)));
        f32 e = sinf(CORE_PI * s);
        vec2 q = vec2_add(p, vec2_mul(n, u * (0.3f + 1.6f * e)));
        vec2 pt = vec2_add(C, vec2_mul(q, R));
        f32 tw = 0.5f + 0.5f * sinf(t * (1.0f + 3.0f * hash01(i + 99)) + (f32)i);
        f32 sz = S(0.6f + 1.4f * hash01(i * 11 + 3));
        f32 a = (0.25f + 0.75f * tw * tw) * alpha * (0.7f + 0.6f * sp->treble);
        vec3 c = vec3_lerp(ribbon_color(s, sc), v3(1, 1, 1), 0.6f);
        core_draw_circle_ex(r, pt, sz, 0, sz * 1.5f, ui_rgb(c, a));
    }
    core_set_blend(r, CORE_BLEND_NORMAL);
}

/* Radial spectrum: bars fan out from the progress ring, mirrored left/right
   (bass at the top, treble at the bottom), each with a falling peak dot. */
static void draw_bars(App *app, vec2 C, f32 R, Scene_Colors sc, f32 alpha) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    Spectrum *sp = &app->spec;
    enum { BARS = SPECTRUM_BANDS * 2 };
    f32 r0 = R + S(16);
    f32 len_max = R * 0.36f * (1.0f + 0.12f * sp->beat);
    f32 th = CORE_MAX(1.5f, S(4.2f));
    for (u32 i = 0; i < BARS; i++) {
        u32 band = i < SPECTRUM_BANDS ? i : BARS - 1 - i;
        f32 ang = -CORE_PI * 0.5f + ((f32)i + 0.5f) / (f32)BARS * CORE_TAU;
        vec2 d = vec2_make(cosf(ang), sinf(ang));
        f32 v = sp->bands[band], pk = sp->peaks[band];
        f32 len = S(3) + len_max * v;
        vec2 a = vec2_add(C, vec2_mul(d, r0));
        vec2 b = vec2_add(C, vec2_mul(d, r0 + len));
        vec3 c = ribbon_color((f32)band / (SPECTRUM_BANDS - 1), sc);
        core_set_blend(r, CORE_BLEND_ADD);
        core_draw_line_ex(r, a, b, th * 3.0f, th * 2.0f, ui_rgb(c, 0.10f * alpha * (0.4f + v)),
                          ui_rgb(c, 0.16f * alpha * (0.4f + v)));
        core_set_blend(r, CORE_BLEND_NORMAL);
        core_draw_line_ex(r, a, b, th, 0, ui_rgb(c, (0.55f + 0.3f * v) * alpha),
                          ui_rgb(vec3_lerp(c, v3(1, 1, 1), 0.35f), (0.75f + 0.25f * v) * alpha));
        if (pk > 0.02f) {
            vec2 p = vec2_add(C, vec2_mul(d, r0 + S(3) + len_max * pk + S(6)));
            core_draw_circle(r, p, th * 0.45f, ui_rgb(vec3_lerp(c, v3(1, 1, 1), 0.5f), 0.8f * alpha));
        }
    }
}

/* Progress ring around the stage; the knob can be dragged to seek. */
static void draw_ring(App *app, vec2 C, f32 R, f32 progress, f64 duration) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    const Platform_Input *in = ui->in;

    vec2 m = in->mouse_pos;
    f32 d = vec2_distance(m, C);
    u64 id = ui_id("ring");
    b32 near = ui->input_enabled && fabsf(d - R) < S(14) && duration > 0;
    if (near && !ui->active) {
        ui->hot = id;
        ui->cursor = PLATFORM_CURSOR_HAND;
        if (in->mouse_pressed[PLATFORM_MOUSE_LEFT] && !ui->mouse_taken) {
            ui->active = id;
            ui->mouse_taken = true;
        }
    }
    b32 dragging = ui->active == id && in->mouse_down[PLATFORM_MOUSE_LEFT];
    if (ui->active == id) {
        f32 ang = atan2f(m.y - C.y, m.x - C.x) + CORE_PI * 0.5f;
        if (ang < 0) ang += CORE_TAU;
        f32 pr = ang / CORE_TAU;
        app->seek_preview = pr;
        if (!in->mouse_down[PLATFORM_MOUSE_LEFT]) {
            player_seek(app->player, pr * duration);
            app->seek_hold_until = app->now + 0.25;
            ui->active = 0;
        }
    }
    if (dragging || app->now < app->seek_hold_until) progress = app->seek_preview;

    f32 hover = ui_ease(ui, ui_idx(id, 1), (near || dragging) ? 1.0f : 0.0f, 14.0f);
    core_draw_circle_ex(r, C, R, S(1.2f), 0, UI_RGBA(255, 255, 255, 0.07f + 0.05f * hover));
    f32 a0 = -CORE_PI * 0.5f;
    f32 sweep = CORE_TAU * clamp01(progress);
    core_set_blend(r, CORE_BLEND_ADD);
    core_draw_arc(r, C, R, S(6), a0, sweep, UI_RGBA(168, 85, 247, 0.10f));
    core_set_blend(r, CORE_BLEND_NORMAL);
    core_draw_arc(r, C, R, S(1.8f + 0.8f * hover), a0, sweep, UI_RGBA(176, 106, 255, 0.95f));
    f32 ang = a0 + sweep;
    vec2 knob = vec2_make(C.x + cosf(ang) * R, C.y + sinf(ang) * R);
    f32 kr = S(4.5f + 2.5f * hover + 1.5f * app->spec.beat);
    core_set_blend(r, CORE_BLEND_ADD);
    core_draw_circle_ex(r, knob, kr * 2.2f, 0, kr * 2.2f, UI_RGBA(190, 140, 255, 0.35f));
    core_set_blend(r, CORE_BLEND_NORMAL);
    core_draw_circle(r, knob, kr, UI_RGBA(255, 255, 255, 1));
}

static void draw_cover_card(App *app, vec2 center, f32 size) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    Cover_Art art = covers_art(app->covers);
    Cover_Art prev = covers_art_prev(app->covers);
    f32 pulse = 1.0f + 0.012f * app->spec.beat;
    f32 sz = size * pulse;
    vec2 pos = vec2_make(center.x - sz * 0.5f, center.y - sz * 0.5f);
    vec2 dim = vec2_make(sz, sz);
    f32 rad = S(10);

    core_draw_shadow(r, vec2_make(pos.x, pos.y + S(18)), dim, rad, S(40), UI_RGBA(0, 0, 0, 0.55f));

    /* placeholder base */
    const Lib_Track *t = current_track(app);
    Core_BoxStyle ph = { .radius = rad, .fill = hash_color(t ? t->path_hash : 7, 1),
                         .fill2 = UI_RGBA(20, 16, 36, 1), .gradient = 1 };
    core_draw_box(r, pos, dim, &ph);

    f32 fade = art.ready && !art.missing ? ease_out_cubic(art.age / 0.35f) : 0;
    if (prev.ready && !prev.missing && fade < 1)
        core_draw_image_rounded(r, prev.tex, pos, dim, vec2_zero(), vec2_make(1, 1), rad, -1,
                                (vec4){ .x = 1, .y = 1, .z = 1, .w = 1 - fade });
    if (fade > 0)
        core_draw_image_rounded(r, art.tex, pos, dim, vec2_zero(), vec2_make(1, 1), rad, -1,
                                (vec4){ .x = 1, .y = 1, .z = 1, .w = fade });
    if (fade < 1 && !(prev.ready && !prev.missing)) {
        ui_icon_note(ui, center, sz * 0.22f, UI_RGBA(255, 255, 255, 0.25f * (1 - fade)));
    }
    if (art.ready && art.age < 0.4f) ui->animating = true;

    /* glass edge + top sheen */
    Core_BoxStyle edge = { .radius = rad, .fill = UI_RGBA(0, 0, 0, 0), .border = S(1),
                           .border_color = UI_RGBA(255, 255, 255, 0.10f) };
    core_draw_box(r, pos, dim, &edge);
    Core_BoxStyle sheen = { .radius = rad, .fill = UI_RGBA(255, 255, 255, 0.06f),
                            .fill2 = UI_RGBA(255, 255, 255, 0), .gradient = 1 };
    core_draw_box(r, pos, vec2_make(sz, sz * 0.45f), &sheen);
}

/* ------------------------------------------------------------------------- */
/* stage: title, seek bar, transport                                         */
/* ------------------------------------------------------------------------- */

static void draw_titles(App *app, f32 cx, f32 ty, f32 ay) {
    Ui *ui = &app->ui;
    const Lib_Track *t = current_track(app);
    u64 h = t ? t->path_hash : 0;
    if (h != app->shown_hash) {
        memcpy(app->prev_title, app->shown_title, sizeof(app->prev_title));
        memcpy(app->prev_artist, app->shown_artist, sizeof(app->prev_artist));
        if (t) {
            copy_str(app->shown_title, sizeof(app->shown_title), t->title);
            copy_str(app->shown_artist, sizeof(app->shown_artist), t->artist);
        } else {
            snprintf(app->shown_title, sizeof(app->shown_title), "%s", app->lib ? "Nothing playing" : "Scanning library");
            snprintf(app->shown_artist, sizeof(app->shown_artist), "%s", app->lib ? "Pick a song or press Ctrl K" : "One moment");
        }
        b32 first = app->shown_hash == 0 && app->prev_title[0] == 0;
        app->shown_hash = h;
        ui_anim_set(ui, ui_id("title.swap"), first ? 1.0f : 0.0f);
    }
    f32 k = ui_ease(ui, ui_id("title.swap"), 1.0f, 5.5f);
    f32 e = ease_out_cubic(k);
    f32 max_w = S(560);
    f32 tp = S(26), ap = S(17);
    if (k < 1) {
        f32 o = 1 - e;
        ui_text(ui, ui->font_med, core_str(app->prev_title), cx, ty - S(16) * e, tp,
                ui_alpha(UI_TEXT, o), UI_ALIGN_CENTER, max_w);
        ui_text(ui, ui->font, core_str(app->prev_artist), cx, ay - S(16) * e, ap,
                ui_alpha(UI_LAVENDER, o), UI_ALIGN_CENTER, max_w);
    }
    ui_text(ui, ui->font_med, core_str(app->shown_title), cx, ty + S(18) * (1 - e), tp,
            ui_alpha(UI_TEXT, e), UI_ALIGN_CENTER, max_w);
    f32 e2 = ease_out_cubic(clamp01(k * 1.25f - 0.25f));
    ui_text(ui, ui->font, core_str(app->shown_artist), cx, ay + S(18) * (1 - e2), ap,
            ui_alpha(UI_LAVENDER, e2), UI_ALIGN_CENTER, max_w);
}

static void draw_heart(App *app, vec2 c) {
    Ui *ui = &app->ui;
    const Lib_Track *t = current_track(app);
    if (!t) return;
    u64 id = ui_id("heart");
    f32 hs = S(26);
    Ui_Interact it = ui_interact(ui, id, vec2_make(c.x - hs * 0.5f, c.y - hs * 0.5f), vec2_make(hs, hs),
                                 PLATFORM_CURSOR_HAND);
    b32 liked = is_liked(app, t->path_hash);
    if (it.clicked) {
        liked = !liked;
        set_liked(app, t->path_hash, liked);
        ui_anim_kick(ui, ui_idx(id, 7), liked ? 14.0f : -6.0f);
        if (liked) ui_burst(ui, c, 10, UI_RGBA(190, 120, 255, 1), 1, S(90));
    }
    f32 pop = ui_spring(ui, ui_idx(id, 7), 0, 260, 12);
    f32 fill = ui_ease(ui, ui_idx(id, 8), liked ? 1.0f : 0.0f, 16.0f);
    f32 sz = S(15) * (1 + 0.06f * pop + 0.12f * it.hover_t - 0.1f * it.press_t);
    vec4 col = ui_mix(UI_RGBA(150, 140, 180, 0.9f), UI_RGBA(157, 102, 255, 1), fill);
    if (fill > 0.02f) {
        core_set_blend(app->r, CORE_BLEND_ADD);
        core_draw_circle_ex(app->r, c, sz * 0.9f, 0, sz, UI_RGBA(150, 90, 255, 0.25f * fill));
        core_set_blend(app->r, CORE_BLEND_NORMAL);
        ui_icon_heart(ui, c, sz * (0.6f + 0.4f * fill), col);
    }
    if (fill < 0.98f) ui_icon_heart_outline(ui, c, sz, S(1.5f), ui_alpha(col, 1 - fill));
}

static void draw_seek_bar(App *app, f32 x0, f32 x1, f32 y, Player_Status *st) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    u64 id = ui_id("seek");
    f32 dur = (f32)st->duration_s;
    Ui_Interact it = ui_interact(ui, id, vec2_make(x0 - S(6), y - S(12)), vec2_make(x1 - x0 + S(12), S(24)),
                                 PLATFORM_CURSOR_HAND);
    f32 progress = dur > 0 ? (f32)(st->position_s / dur) : 0;
    f32 mx = clamp01((ui->in->mouse_pos.x - x0) / (x1 - x0));
    if (it.held && dur > 0) {
        app->seeking = true;
        app->seek_preview = mx;
    }
    if (app->seeking && !it.held) {
        app->seeking = false;
        if (dur > 0) {
            player_seek(app->player, app->seek_preview * dur);
            app->seek_hold_until = app->now + 0.25;
        }
    }
    if (app->seeking || app->now < app->seek_hold_until) progress = app->seek_preview;

    f32 grow = ui_ease(ui, ui_idx(id, 3), (it.hovered || it.held) ? 1.0f : 0.0f, 16.0f);
    f32 th = S(3.0f + 2.0f * grow);
    f32 px = x0 + (x1 - x0) * clamp01(progress);
    core_draw_rect_rounded(r, vec2_make(x0, y - th * 0.5f), vec2_make(x1 - x0, th), th * 0.5f,
                           UI_RGBA(255, 255, 255, 0.16f));
    Core_BoxStyle fill = { .radius = th * 0.5f, .fill = UI_RGBA(236, 232, 255, 1),
                           .fill2 = UI_RGBA(255, 255, 255, 1), .gradient = 2 };
    if (px > x0) core_draw_box(r, vec2_make(x0, y - th * 0.5f), vec2_make(px - x0, th), &fill);
    /* hover ghost */
    if (grow > 0.01f && !it.held) {
        f32 gx = x0 + (x1 - x0) * mx;
        core_draw_circle(r, vec2_make(gx, y), S(3) * grow, UI_RGBA(255, 255, 255, 0.35f * grow));
        Core_String tt = fmt_time(app, mx * dur);
        f32 tw = ui_text_width(ui, ui->font_mono, tt, S(12)) + S(14);
        vec2 bp = vec2_make(gx - tw * 0.5f, y - S(34));
        Core_BoxStyle tip = { .radius = S(6), .fill = UI_RGBA(30, 26, 48, 0.92f * grow),
                              .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.1f * grow) };
        core_draw_box(r, bp, vec2_make(tw, S(22)), &tip);
        ui_text(ui, ui->font_mono, tt, gx, bp.y + S(11), S(12), ui_alpha(UI_TEXT, grow), UI_ALIGN_CENTER, 0);
    }
    f32 kr = S(6.5f + 2.0f * grow);
    core_set_blend(r, CORE_BLEND_ADD);
    core_draw_circle_ex(r, vec2_make(px, y), kr * 2.4f, 0, kr * 2.0f, UI_RGBA(180, 150, 255, 0.28f + 0.2f * grow));
    core_set_blend(r, CORE_BLEND_NORMAL);
    core_draw_circle(r, vec2_make(px, y), kr, UI_RGBA(255, 255, 255, 1));

    f64 shown_pos = (app->seeking || app->now < app->seek_hold_until) ? app->seek_preview * dur : st->position_s;
    ui_text(ui, ui->font, fmt_time(app, shown_pos), x0 - S(19), y, S(17), UI_RGBA(222, 219, 236, 1), UI_ALIGN_RIGHT, 0);
    ui_text(ui, ui->font, fmt_time(app, dur), x1 + S(19), y, S(17), UI_RGBA(222, 219, 236, 1), UI_ALIGN_LEFT, 0);
}

/* A round icon button with hover halo, press squash and a click ripple. */
static Ui_Interact icon_button(App *app, const char *name, vec2 c, f32 radius) {
    Ui *ui = &app->ui;
    u64 id = ui_id(name);
    Ui_Interact it = ui_interact(ui, id, vec2_make(c.x - radius, c.y - radius), vec2_make(radius * 2, radius * 2),
                                 PLATFORM_CURSOR_HAND);
    if (it.hover_t > 0.01f)
        core_draw_circle(app->r, c, radius * (0.8f + 0.2f * it.hover_t), UI_RGBA(255, 255, 255, 0.06f * it.hover_t));
    if (it.pressed) {
        ui_ripple(ui, c, radius * 1.1f, UI_RGBA(190, 160, 255, 0.35f),
                  vec2_make(c.x - radius * 1.5f, c.y - radius * 1.5f), vec2_make(radius * 3, radius * 3));
        ui_anim_kick(ui, ui_idx(id, 9), -9.0f);
    }
    return it;
}

static f32 button_scale(App *app, const char *name, Ui_Interact it) {
    Ui *ui = &app->ui;
    f32 pop = ui_spring(ui, ui_idx(ui_id(name), 9), 0, 320, 14);
    return 1.0f + 0.08f * it.hover_t + 0.03f * pop;
}

static void draw_transport(App *app, f32 cx, f32 y, Player_Status *st) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    f32 gap = S(111);
    vec4 icon = UI_RGBA(236, 233, 248, 1);
    vec4 icon_dim = UI_RGBA(200, 196, 220, 1);

    /* shuffle */
    vec2 c = vec2_make(cx - gap * 2, y);
    Ui_Interact it = icon_button(app, "btn.shuffle", c, S(22));
    if (it.clicked) set_shuffle(app, !app->shuffle);
    f32 on = ui_ease(ui, ui_id("shuffle.on"), app->shuffle ? 1.0f : 0.0f, 14.0f);
    ui_icon_shuffle(ui, c, S(26) * button_scale(app, "btn.shuffle", it), ui_mix(icon_dim, UI_ACCENT_BRIGHT, on));
    if (on > 0.01f) core_draw_circle(r, vec2_make(c.x, c.y + S(20)), S(2.2f) * on, ui_alpha(UI_ACCENT_BRIGHT, on));

    /* prev */
    c = vec2_make(cx - gap, y);
    it = icon_button(app, "btn.prev", c, S(24));
    if (it.clicked) skip(app, -1);
    ui_icon_prev(ui, c, S(26) * button_scale(app, "btn.prev", it), icon);

    /* play / pause: the hero button */
    c = vec2_make(cx, y);
    u64 pid = ui_id("btn.play");
    f32 pr = S(40);
    it = ui_interact(ui, pid, vec2_make(c.x - pr, c.y - pr), vec2_make(pr * 2, pr * 2), PLATFORM_CURSOR_HAND);
    if (it.clicked) {
        toggle_pause(app);
        ui_anim_kick(ui, ui_idx(pid, 9), 7.0f);
        ui_ripple(ui, c, pr * 1.9f, UI_RGBA(170, 130, 255, 0.5f), vec2_make(c.x - pr * 3, c.y - pr * 3), vec2_make(pr * 6, pr * 6));
    }
    f32 pop = ui_spring(ui, ui_idx(pid, 9), 0, 300, 11);
    f32 sc = 1.0f + 0.05f * it.hover_t - 0.07f * it.press_t + 0.04f * pop;
    b32 playing = st->loaded && !st->paused && !st->ended;
    f32 glow = 0.35f + 0.25f * it.hover_t + (playing ? 0.25f * app->spec.level + 0.3f * app->spec.beat : 0);
    core_set_blend(r, CORE_BLEND_ADD);
    core_draw_circle_ex(r, c, pr * sc * 1.05f, 0, pr * 0.9f, UI_RGBA(120, 80, 255, 0.35f * glow));
    core_set_blend(r, CORE_BLEND_NORMAL);
    f32 R = pr * sc;
    Core_BoxStyle disc = { .radius = R, .fill = UI_RGBA(128, 98, 236, 1), .fill2 = UI_RGBA(92, 66, 200, 1), .gradient = 1 };
    core_draw_box(r, vec2_make(c.x - R, c.y - R), vec2_make(R * 2, R * 2), &disc);
    core_draw_circle_ex(r, c, R - S(0.5f), S(1), 0, UI_RGBA(255, 255, 255, 0.14f));
    f32 morph = ui_ease(ui, ui_id("play.morph"), playing ? 1.0f : 0.0f, 16.0f);
    if (morph > 0.02f) ui_icon_pause(ui, c, S(30) * sc * (0.7f + 0.3f * morph), ui_alpha(icon, morph));
    if (morph < 0.98f) ui_icon_play(ui, c, S(30) * sc * (1.0f - 0.3f * morph), ui_alpha(icon, 1 - morph));

    /* next */
    c = vec2_make(cx + gap, y);
    it = icon_button(app, "btn.next", c, S(24));
    if (it.clicked) skip(app, +1);
    ui_icon_next(ui, c, S(26) * button_scale(app, "btn.next", it), icon);

    /* repeat */
    c = vec2_make(cx + gap * 2, y);
    it = icon_button(app, "btn.repeat", c, S(22));
    if (it.clicked) { app->repeat = (app->repeat + 1) % 3; app->next_sent = false; sync_next(app); app->state_dirty = true; }
    f32 ron = ui_ease(ui, ui_id("repeat.on"), app->repeat ? 1.0f : 0.0f, 14.0f);
    ui_icon_repeat(ui, c, S(26) * button_scale(app, "btn.repeat", it), ui_mix(icon_dim, UI_ACCENT_BRIGHT, ron),
                   app->repeat == REPEAT_ONE);
    if (ron > 0.01f) core_draw_circle(r, vec2_make(c.x, c.y + S(20)), S(2.2f) * ron, ui_alpha(UI_ACCENT_BRIGHT, ron));
}

static void draw_volume_toast(App *app, f32 cx, f32 y) {
    Ui *ui = &app->ui;
    f32 vis = ui_ease(ui, ui_id("vol.toast"), app->volume_toast > 0 ? 1.0f : 0.0f, 12.0f);
    if (vis < 0.01f) return;
    f32 w = S(210), h = S(40);
    vec2 p = vec2_make(cx - w * 0.5f, y - h * 0.5f + S(10) * (1 - vis));
    Core_BoxStyle st = { .radius = h * 0.5f, .fill = UI_RGBA(28, 24, 46, 0.94f * vis),
                         .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.10f * vis) };
    core_draw_shadow(app->r, p, vec2_make(w, h), h * 0.5f, S(16), UI_RGBA(0, 0, 0, 0.4f * vis));
    core_draw_box(app->r, p, vec2_make(w, h), &st);
    ui_icon_volume(ui, vec2_make(p.x + S(24), p.y + h * 0.5f), S(18), app->volume, ui_alpha(UI_TEXT, vis));
    f32 bx0 = p.x + S(44), bx1 = p.x + w - S(56);
    f32 vol = ui_ease(ui, ui_id("vol.shown"), app->volume, 20.0f);
    core_draw_rect_rounded(app->r, vec2_make(bx0, p.y + h * 0.5f - S(2)), vec2_make(bx1 - bx0, S(4)), S(2), UI_RGBA(255, 255, 255, 0.15f * vis));
    core_draw_rect_rounded(app->r, vec2_make(bx0, p.y + h * 0.5f - S(2)), vec2_make((bx1 - bx0) * vol, S(4)), S(2), ui_alpha(UI_ACCENT_BRIGHT, vis));
    ui_text(ui, ui->font_mono, str_fmt(app, "%d%%", (int)(app->volume * 100 + 0.5f)), p.x + w - S(18), p.y + h * 0.5f,
            S(13), ui_alpha(UI_TEXT, vis), UI_ALIGN_RIGHT, 0);
}

/* ------------------------------------------------------------------------- */
/* panel                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    u32 first, last;   /* visible index range [first, last) */
    f32 y0;            /* y of item 0 (scroll applied) */
    f32 row_h;
    vec2 pos, size;
    Scroll *sc;
    u32 count;
} List_View;

static List_View list_begin(App *app, Scroll *sc, u64 id, vec2 pos, vec2 size, u32 count, f32 row_h, f32 pad_top) {
    Ui *ui = &app->ui;
    const Platform_Input *in = ui->in;
    f32 content = pad_top + row_h * (f32)count + S(12);
    f32 max_scroll = CORE_MAX(0.0f, content - size.y);

    if (ui_mouse_in(ui, pos, size) && in->scroll_y != 0) {
        f32 step = in->scroll_precise ? in->scroll_y * S(12) : in->scroll_y * row_h * 2.0f;
        sc->target -= step;
        sc->idle = 0;
        app->queue_follow = false;
        if (in->scroll_precise) sc->pos = CORE_CLAMP(sc->target, 0.0f, max_scroll);
    }
    /* scrollbar drag */
    f32 bar_w = S(4);
    vec2 track_pos = vec2_make(pos.x + size.x - bar_w - S(2), pos.y + S(4));
    f32 track_h = size.y - S(8);
    f32 thumb_h = max_scroll > 0 ? CORE_MAX(S(28), track_h * size.y / content) : track_h;
    Ui_Interact bar = ui_interact(ui, ui_idx(id, 0xBA), vec2_make(track_pos.x - S(6), track_pos.y),
                                  vec2_make(bar_w + S(10), track_h), PLATFORM_CURSOR_DEFAULT);
    if (max_scroll > 0 && bar.held) {
        f32 rel = (in->mouse_pos.y - track_pos.y - thumb_h * 0.5f) / CORE_MAX(1.0f, track_h - thumb_h);
        sc->target = clamp01(rel) * max_scroll;
        sc->pos = sc->target;
        sc->idle = 0;
        app->queue_follow = false;
    }

    sc->target = CORE_CLAMP(sc->target, 0.0f, max_scroll);
    f32 k = 1.0f - expf(-ui->dt * 20.0f);
    sc->pos += (sc->target - sc->pos) * k;
    if (fabsf(sc->target - sc->pos) < 0.25f) sc->pos = sc->target;
    else ui->animating = true;
    sc->idle += ui->dt;
    f32 show = (sc->idle < 0.9f || bar.hovered || bar.held || ui_mouse_in(ui, vec2_make(track_pos.x - S(10), pos.y), vec2_make(S(20), size.y))) ? 1.0f : 0.0f;
    sc->bar_t = ui_ease(ui, ui_idx(id, 0xBB), max_scroll > 0 ? show : 0.0f, 10.0f);

    List_View lv = { .row_h = row_h, .pos = pos, .size = size, .sc = sc, .count = count };
    lv.y0 = pos.y + pad_top - sc->pos;
    f32 first = floorf((pos.y - lv.y0) / row_h);
    if (first < 0) first = 0;
    lv.first = (u32)first;
    lv.last = CORE_MIN(count, lv.first + (u32)(size.y / row_h) + 2);
    core_clip_push(app->r, pos, size);

    if (max_scroll > 0 && sc->bar_t > 0.01f) {
        /* drawn later in list_end, stash geometry in the scroll */
    }
    return lv;
}

static void list_end(App *app, List_View *lv) {
    Ui *ui = &app->ui;
    core_clip_pop(app->r);
    Scroll *sc = lv->sc;
    f32 content = (lv->y0 + sc->pos - lv->pos.y) + lv->row_h * (f32)lv->count + S(12);
    f32 max_scroll = CORE_MAX(0.0f, content - lv->size.y);
    if (max_scroll <= 0 || sc->bar_t < 0.01f) return;
    f32 bar_w = S(4);
    f32 track_h = lv->size.y - S(8);
    f32 thumb_h = CORE_MAX(S(28), track_h * lv->size.y / content);
    f32 ty = lv->pos.y + S(4) + (track_h - thumb_h) * (sc->pos / max_scroll);
    core_draw_rect_rounded(app->r, vec2_make(lv->pos.x + lv->size.x - bar_w - S(2), ty), vec2_make(bar_w, thumb_h),
                           bar_w * 0.5f, UI_RGBA(255, 255, 255, 0.22f * sc->bar_t));
    CORE_UNUSED(ui);
}

static void draw_thumb(App *app, const Lib_Track *t, vec2 pos, f32 size, f32 radius, f32 alpha) {
    Ui *ui = &app->ui;
    Cover_Thumb th = covers_thumb(app->covers, t);
    f32 fade = th.ready ? ease_out_cubic(th.age / 0.25f) : 0;
    if (th.ready && th.age < 0.3f) ui->animating = true;
    if (fade < 1) {
        Core_BoxStyle ph = { .radius = radius, .fill = hash_color(t->path_hash, alpha),
                             .fill2 = UI_RGBA(24, 20, 40, alpha), .gradient = 1 };
        core_draw_box(app->r, pos, vec2_make(size, size), &ph);
        if (th.missing || !th.ready)
            ui_icon_note(ui, vec2_make(pos.x + size * 0.5f, pos.y + size * 0.5f), size * 0.3f,
                         UI_RGBA(255, 255, 255, 0.22f * alpha));
    }
    if (th.ready && !th.missing)
        core_draw_image_rounded(app->r, th.tex, pos, vec2_make(size, size), th.uv0, th.uv1, radius, -1,
                                (vec4){ .x = 1, .y = 1, .z = 1, .w = fade * alpha });
}

typedef struct { b32 clicked; b32 menu; b32 double_clicked; } Row_Result;

/* One track row: thumb, title, artist, kebab. `current` gets the accent box.
   `show_duration` adds the track length left of the kebab. */
static Row_Result track_row(App *app, u64 id, vec2 pos, vec2 size, const Lib_Track *t, b32 current,
                            b32 playing, f32 appear, b32 show_duration) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    Row_Result res = {0};
    /* kebab first: it sits on top of the row and must claim the click before the row does */
    f32 kebab_x = pos.x + size.x - S(15);
    vec2 kc = vec2_make(kebab_x, pos.y + size.y * 0.5f);
    Ui_Interact kb = {0};
    if (!current)
        kb = ui_interact(ui, ui_idx(id, 0x4B), vec2_make(kc.x - S(14), kc.y - S(16)), vec2_make(S(28), S(32)),
                         PLATFORM_CURSOR_HAND);
    Ui_Interact it = ui_interact(ui, id, pos, size, PLATFORM_CURSOR_HAND);
    f32 cur_t = ui_ease(ui, ui_idx(id, 0xC0), current ? 1.0f : 0.0f, 12.0f);

    if (it.pressed) {
        ui_ripple(ui, ui->in->mouse_pos, size.x * 0.6f, UI_RGBA(160, 130, 255, 0.18f), pos, size);
    }
    f32 press = it.press_t;
    vec2 p = vec2_make(pos.x + press * S(2), pos.y);

    if (cur_t > 0.01f) {
        core_draw_shadow(r, pos, size, S(9), S(14), UI_RGBA(124, 80, 255, 0.18f * cur_t));
        Core_BoxStyle st = { .radius = S(9), .fill = UI_RGBA(120, 90, 250, 0.13f * cur_t),
                             .border = S(1.3f), .border_color = UI_RGBA(139, 102, 255, 0.85f * cur_t) };
        core_draw_box(r, pos, size, &st);
    }
    if (it.hover_t > 0.01f && cur_t < 0.99f)
        core_draw_rect_rounded(r, pos, size, S(9), UI_RGBA(255, 255, 255, 0.045f * it.hover_t * (1 - cur_t)));

    f32 th = S(50);
    f32 a = appear;
    draw_thumb(app, t, vec2_make(p.x + S(13), p.y + (size.y - th) * 0.5f), th, S(6), a);

    f32 tx = p.x + S(86);
    f32 max_w = kebab_x - S(22) - tx;
    Core_String dur = {0};
    if (show_duration && t->duration_ms) {
        dur = fmt_time(app, (f64)t->duration_ms / 1000.0);
        f32 dw = ui_text_width(ui, ui->font, dur, S(13));
        ui_text(ui, ui->font, dur, kebab_x - S(22), p.y + size.y * 0.5f, S(13),
                ui_mix(ui_alpha(UI_TEXT_DIM, a), ui_alpha(UI_RGBA(170, 160, 210, 1), a), cur_t), UI_ALIGN_RIGHT, 0);
        max_w -= dw + S(12);
    }
    vec4 title_col = ui_mix(UI_RGBA(232, 229, 244, a), UI_RGBA(236, 230, 255, a), cur_t);
    ui_text(ui, ui->font, t->title, tx, p.y + size.y * 0.5f - S(9.5f), S(15), title_col, UI_ALIGN_LEFT, max_w);
    ui_text(ui, ui->font, t->artist, tx, p.y + size.y * 0.5f + S(11), S(13),
            ui_mix(ui_alpha(UI_TEXT_DIM, a), ui_alpha(UI_RGBA(170, 160, 210, 1), a), cur_t), UI_ALIGN_LEFT, max_w);

    /* right side: eq bars when current, kebab otherwise */
    if (current) {
        f32 lv[4];
        for (u32 i = 0; i < 4; i++) {
            f32 b = app->spec.bands[4 + i * 7];
            lv[i] = playing ? 0.25f + 0.75f * b : 0.3f + 0.1f * (f32)((i * 5) % 3);
        }
        ui_icon_eq(ui, kc, S(18), lv, UI_RGBA(160, 120, 255, a));
    } else {
        if (kb.hover_t > 0.01f) core_draw_circle(r, kc, S(14), UI_RGBA(255, 255, 255, 0.07f * kb.hover_t));
        ui_icon_kebab(ui, kc, S(14), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, kb.hover_t), a));
        if (kb.clicked) res.menu = true;
    }
    if (it.clicked) res.clicked = true;
    if (it.right_clicked) res.menu = true;
    if (it.pressed && ui->double_click) res.double_clicked = true;
    return res;
}

static void open_settings(App *app, b32 open);

static void open_menu(App *app, u32 track, s32 queue_index, b32 artist_actions) {
    app->menu = MENU_TRACK;
    app->menu_track = track;
    app->menu_queue_index = queue_index;
    app->menu_artist_actions = artist_actions;
    app->menu_pos = app->ui.in->mouse_pos;
    ui_anim_set(&app->ui, ui_id("menu.t"), 0);
}

static void open_artist_menu(App *app, u32 artist) {
    app->menu = MENU_ARTIST;
    app->menu_artist = artist;
    app->menu_pos = app->ui.in->mouse_pos;
    ui_anim_set(&app->ui, ui_id("menu.t"), 0);
}

static void open_genre_menu(App *app, u32 genre) {
    app->menu = MENU_GENRE;
    app->menu_genre = genre;
    app->menu_pos = app->ui.in->mouse_pos;
    ui_anim_set(&app->ui, ui_id("menu.t"), 0);
}

static void draw_tab_bar(App *app, f32 x0, f32 y, f32 panel_w) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    static const char *labels[TAB_COUNT] = { "Queue", "Genres", "Artists", "Songs" };
    f32 item_x[TAB_COUNT], item_w[TAB_COUNT], lw[TAB_COUNT];
    f32 px = S(15);
    /* Everything must end before the gear's separator. With all labels shown
       when they fit; otherwise only the active tab keeps its label. */
    f32 x_start = x0 + S(36), x_end = x0 + panel_w - S(85);
    f32 labelled = 0;
    for (u32 i = 0; i < TAB_COUNT; i++) {
        lw[i] = ui_text_width(ui, ui->font, core_str(labels[i]), px);
        labelled += S(30) + lw[i];
    }
    b32 compact = x_start + labelled + S(20) * (TAB_COUNT - 1) > x_end;
    f32 gap = compact ? S(22) : CORE_MIN(S(38), (x_end - x_start - labelled) / (TAB_COUNT - 1));
    f32 x = x_start;
    for (u32 i = 0; i < TAB_COUNT; i++) {
        Core_String label = core_str(labels[i]);
        u64 id = ui_idx(ui_id("tab"), i);
        f32 act = ui_ease(ui, ui_idx(id, 1), app->tab == i ? 1.0f : 0.0f, 14.0f);
        f32 reveal = compact ? act : 1.0f;
        f32 w = S(22) + reveal * (S(8) + lw[i]);
        Ui_Interact it = ui_interact(ui, id, vec2_make(x - S(12), y - S(22)), vec2_make(w + S(24), S(44)),
                                     PLATFORM_CURSOR_HAND);
        if (it.clicked) {
            if (app->tab == i && i == TAB_ARTISTS) app->artist_open = -1;
            if (app->tab == i && i == TAB_GENRES) app->genre_open = -1;
            if (app->tab == i && i == TAB_QUEUE) app->queue_follow = true;
            app->tab = i;
            app->state_dirty = true;
        }
        vec4 col = ui_mix(ui_mix(UI_TEXT_DIM, UI_RGBA(210, 206, 228, 1), it.hover_t), UI_TEXT, act);
        vec2 ic = vec2_make(x + S(11), y);
        f32 is = S(22) * (1 - 0.08f * it.press_t);
        if (i == TAB_QUEUE) ui_icon_note(ui, ic, is, col);
        else if (i == TAB_GENRES) ui_icon_folder(ui, ic, is, col);
        else if (i == TAB_ARTISTS) ui_icon_person(ui, ic, is, col);
        else ui_icon_disc(ui, ic, is, col);
        if (reveal > 0.02f)
            ui_text(ui, ui->font, label, x + S(30), y, px, ui_alpha(col, col.w * reveal), UI_ALIGN_LEFT, 0);
        item_x[i] = x;
        item_w[i] = w;
        x += w + gap;
    }
    /* sliding underline (spring) */
    f32 ux = ui_spring(ui, ui_id("tab.ux"), item_x[app->tab] - S(12), 380, 30);
    f32 uw = ui_spring(ui, ui_id("tab.uw"), item_w[app->tab] + S(25), 380, 30);
    f32 uy = y + S(25);
    core_draw_rect(r, vec2_make(x0 + S(16), uy + S(1)), vec2_make(panel_w - S(32), S(1)), UI_RGBA(255, 255, 255, 0.05f));
    core_set_blend(r, CORE_BLEND_ADD);
    core_draw_rect_rounded(r, vec2_make(ux, uy - S(3)), vec2_make(uw, S(8)), S(4), UI_RGBA(139, 92, 246, 0.25f));
    core_set_blend(r, CORE_BLEND_NORMAL);
    core_draw_rect_rounded(r, vec2_make(ux, uy - S(1)), vec2_make(uw, S(3)), S(1.5f), UI_RGBA(150, 110, 255, 1));

    /* settings gear */
    vec2 gc = vec2_make(x0 + panel_w - S(37), y);
    Ui_Interact g = ui_interact(ui, ui_id("gear"), vec2_make(gc.x - S(16), gc.y - S(16)), vec2_make(S(32), S(32)),
                                PLATFORM_CURSOR_HAND);
    if (g.clicked) open_settings(app, true);
    f32 lit = ui_ease(ui, ui_id("gear.lit"), app->settings_open ? 1.0f : 0.0f, 8.0f);
    if (g.hover_t > 0.01f) core_draw_circle(r, gc, S(16), UI_RGBA(255, 255, 255, 0.06f * g.hover_t));
    ui_icon_gear(ui, gc, S(21), ui_mix(UI_TEXT_DIM, UI_TEXT, CORE_MAX(g.hover_t, lit)));
    /* vertical separator before the gear */
    core_draw_rect(r, vec2_make(gc.x - S(31), y - S(12)), vec2_make(S(1), S(24)), UI_RGBA(255, 255, 255, 0.08f));
}

static void draw_window_buttons(App *app, f32 right, f32 y) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    const char *names[3] = { "win.min", "win.max", "win.close" };
    f32 xs[3] = { right - S(110), right - S(69), right - S(29) };
    for (u32 i = 0; i < 3; i++) {
        vec2 c = vec2_make(xs[i], y);
        Ui_Interact it = ui_interact(ui, ui_id(names[i]), vec2_make(c.x - S(17), c.y - S(15)), vec2_make(S(34), S(30)),
                                     PLATFORM_CURSOR_DEFAULT);
        vec4 bg = i == 2 ? UI_RGBA(232, 72, 96, 0.9f * it.hover_t) : UI_RGBA(255, 255, 255, 0.08f * it.hover_t);
        if (it.hover_t > 0.01f) core_draw_rect_rounded(r, vec2_make(c.x - S(17), c.y - S(13)), vec2_make(S(34), S(26)), S(6), bg);
        vec4 col = ui_mix(UI_RGBA(200, 196, 220, 1), UI_TEXT, it.hover_t);
        if (i == 0) ui_icon_minimize(ui, c, S(13), col);
        else if (i == 1) ui_icon_maximize(ui, c, S(12), col);
        else ui_icon_close(ui, c, S(13), col);
        if (it.clicked) {
            if (i == 0) platform_window_minimize(app->win);
            else if (i == 1) platform_window_toggle_maximize(app->win);
            else platform_window_request_close(app->win);
        }
    }
}

static void draw_skeleton_rows(App *app, vec2 pos, vec2 size, f32 row_h) {
    Ui *ui = &app->ui;
    ui->animating = true;
    u32 n = (u32)(size.y / row_h) + 1;
    for (u32 i = 0; i < n; i++) {
        f32 y = pos.y + (f32)i * row_h;
        f32 wave = 0.5f + 0.5f * sinf((f32)ui->time * 3.0f - (f32)i * 0.5f);
        vec4 c = UI_RGBA(255, 255, 255, 0.04f + 0.03f * wave);
        core_draw_rect_rounded(app->r, vec2_make(pos.x + S(13), y + (row_h - S(50)) * 0.5f), vec2_make(S(50), S(50)), S(6), c);
        core_draw_rect_rounded(app->r, vec2_make(pos.x + S(86), y + row_h * 0.5f - S(14)), vec2_make(S(150 + (i * 37) % 80), S(10)), S(5), c);
        core_draw_rect_rounded(app->r, vec2_make(pos.x + S(86), y + row_h * 0.5f + S(6)), vec2_make(S(80 + (i * 23) % 50), S(8)), S(4), c);
    }
    if (app->scanner) {
        ui_text(ui, ui->font, str_fmt(app, "Scanning %u files", library_scanner_files_seen(app->scanner)),
                pos.x + size.x * 0.5f, pos.y + size.y - S(30), S(13), UI_TEXT_DIM, UI_ALIGN_CENTER, 0);
    }
}

static void draw_queue_list(App *app, vec2 pos, vec2 size, b32 playing) {
    Ui *ui = &app->ui;
    f32 row_h = S(58);
    Scroll *sc = &app->scroll[TAB_QUEUE];
    /* header: track count + clear */
    f32 hh = S(34);
    ui_text(ui, ui->font, str_fmt(app, "%u track%s", app->queue_len, app->queue_len == 1 ? "" : "s"),
            pos.x + S(14), pos.y + hh * 0.5f, S(13), UI_TEXT_DIM, UI_ALIGN_LEFT, 0);
    if (app->queue_len > 0) {
        Core_String label = core_str("Clear");
        f32 px = S(13);
        f32 bw = ui_text_width(ui, ui->font, label, px) + S(38), bh = S(26);
        vec2 bp = vec2_make(pos.x + size.x - S(8) - bw, pos.y + (hh - bh) * 0.5f);
        Ui_Interact c = ui_interact(ui, ui_id("queue.clear"), bp, vec2_make(bw, bh), PLATFORM_CURSOR_HAND);
        if (c.hover_t > 0.01f) core_draw_rect_rounded(app->r, bp, vec2_make(bw, bh), bh * 0.5f, UI_RGBA(255, 255, 255, 0.07f * c.hover_t));
        vec4 col = ui_mix(UI_TEXT_DIM, UI_TEXT, c.hover_t);
        ui_icon_close(ui, vec2_make(bp.x + S(16), bp.y + bh * 0.5f), S(14), col);
        ui_text(ui, ui->font, label, bp.x + S(28), bp.y + bh * 0.5f, px, col, UI_ALIGN_LEFT, 0);
        if (c.clicked) queue_clear(app);
    }
    pos.y += hh;
    size.y -= hh;
    if (app->queue_follow && app->cur >= 0) {
        sc->target = (f32)app->cur * row_h;
        app->queue_follow = false;
    }
    List_View lv = list_begin(app, sc, ui_id("list.queue"), pos, size, app->queue_len, row_h, S(3));
    for (u32 i = lv.first; i < lv.last; i++) {
        const Lib_Track *t = track_at(app, app->queue[i]);
        if (!t) continue;
        f32 y = lv.y0 + (f32)i * row_h;
        b32 cur = (s32)i == app->cur;
        Row_Result rr = track_row(app, ui_idx(ui_id("q.row"), t->path_hash ^ i), vec2_make(pos.x, y + S(1)),
                                  vec2_make(size.x - S(8), row_h - S(2)), t, cur, playing, 1, true);
        if (rr.clicked && !cur) play_index(app, (s32)i, 0, false);
        else if (rr.clicked && cur) toggle_pause(app);
        if (rr.menu) open_menu(app, app->queue[i], (s32)i, false);
    }
    list_end(app, &lv);
    CORE_UNUSED(ui);
}

static void draw_songs_list(App *app, vec2 pos, vec2 size, const u32 *tracks, u32 count, Scroll *sc, u64 list_id, b32 playing) {
    Ui *ui = &app->ui;
    f32 row_h = S(58);
    const Lib_Track *curt = current_track(app);
    List_View lv = list_begin(app, sc, list_id, pos, size, count, row_h, S(3));
    for (u32 i = lv.first; i < lv.last; i++) {
        const Lib_Track *t = track_at(app, tracks[i]);
        if (!t) continue;
        f32 y = lv.y0 + (f32)i * row_h;
        b32 cur = curt && curt == t;
        Row_Result rr = track_row(app, ui_idx(list_id, t->path_hash), vec2_make(pos.x, y + S(1)),
                                  vec2_make(size.x - S(8), row_h - S(2)), t, cur, playing, 1, true);
        if (rr.clicked) {
            if (cur) toggle_pause(app);
            else play_tracks(app, tracks, count, i);
        }
        if (rr.menu) open_menu(app, tracks[i], -1, false);
    }
    list_end(app, &lv);
}

/* Artists and genres are the same kind of list: named groups of tracks that
   open into a song list. */
typedef enum { GROUP_ARTIST, GROUP_GENRE } Group_Kind;

static u32 group_count(const Library *lib, Group_Kind k) {
    return k == GROUP_ARTIST ? lib->artist_count : lib->genre_count;
}

static void group_get(const Library *lib, Group_Kind k, u32 i, Core_String *name, const u32 **tracks, u32 *count) {
    if (k == GROUP_ARTIST) {
        *name = lib->artists[i].name; *tracks = lib->artists[i].tracks; *count = lib->artists[i].track_count;
    } else {
        *name = lib->genres[i].name; *tracks = lib->genres[i].tracks; *count = lib->genres[i].track_count;
    }
}

static void draw_groups(App *app, vec2 pos, vec2 size, b32 playing, Group_Kind kind) {
    Ui *ui = &app->ui;
    Library *lib = app->lib;
    b32 artists = kind == GROUP_ARTIST;
    s32 *open = artists ? &app->artist_open : &app->genre_open;
    Scroll *list_scroll = &app->scroll[artists ? TAB_ARTISTS : TAB_GENRES];
    Scroll *open_scroll = &app->scroll[TAB_COUNT + (artists ? 0 : 1)];
    u32 total = group_count(lib, kind);
    if (*open >= 0 && (u32)*open < total) {
        Core_String name;
        const u32 *tracks;
        u32 track_count;
        group_get(lib, kind, (u32)*open, &name, &tracks, &track_count);
        /* header with back button */
        f32 hh = S(46);
        u64 bid = ui_id(artists ? "artist.back" : "genre.back");
        /* kebab first: the first interact under the cursor claims the click */
        vec2 kc = vec2_make(pos.x + size.x - S(23), pos.y + hh * 0.5f);
        Ui_Interact kb = ui_interact(ui, ui_id(artists ? "artist.kebab" : "genre.kebab"), vec2_make(kc.x - S(14), kc.y - S(16)), vec2_make(S(28), S(32)),
                                     PLATFORM_CURSOR_HAND);
        Ui_Interact it = ui_interact(ui, bid, vec2_make(pos.x, pos.y + S(2)), vec2_make(size.x - S(8), hh - S(4)), PLATFORM_CURSOR_HAND);
        if (it.hover_t > 0.01f) core_draw_rect_rounded(app->r, vec2_make(pos.x, pos.y + S(2)), vec2_make(size.x - S(8), hh - S(4)), S(9), UI_RGBA(255, 255, 255, 0.04f * it.hover_t));
        ui_icon_chevron_left(ui, vec2_make(pos.x + S(20) - S(3) * it.hover_t, pos.y + hh * 0.5f), S(22), UI_TEXT);
        ui_text(ui, ui->font_semi, name, pos.x + S(40), pos.y + hh * 0.5f, S(17), UI_TEXT, UI_ALIGN_LEFT, size.x - S(150));
        if (kb.hover_t > 0.01f) core_draw_circle(app->r, kc, S(14), UI_RGBA(255, 255, 255, 0.07f * kb.hover_t));
        ui_icon_kebab(ui, kc, S(14), ui_mix(UI_TEXT_DIM, UI_TEXT, kb.hover_t));
        ui_text(ui, ui->font, str_fmt(app, "%u song%s", track_count, track_count == 1 ? "" : "s"),
                pos.x + size.x - S(46), pos.y + hh * 0.5f, S(13), UI_TEXT_DIM, UI_ALIGN_RIGHT, 0);
        if (kb.clicked || it.right_clicked) {
            if (artists) open_artist_menu(app, (u32)*open); else open_genre_menu(app, (u32)*open);
            return;
        }
        if (it.clicked || ui->in->key_pressed[PLATFORM_KEY_BACKSPACE]) { *open = -1; return; }
        u64 slide_id = ui_id(artists ? "artist.slide" : "genre.slide");
        f32 slide = ui_ease(ui, slide_id, 1.0f, 12.0f);
        vec2 lp = vec2_make(pos.x + S(30) * (1 - slide), pos.y + hh);
        draw_songs_list(app, lp, vec2_make(size.x, size.y - hh), tracks, track_count, open_scroll,
                        ui_idx(ui_id(artists ? "list.artist" : "list.genre"), (u64)*open), playing);
        return;
    }

    f32 row_h = S(58);
    List_View lv = list_begin(app, list_scroll, ui_id(artists ? "list.artists" : "list.genres"), pos, size, total, row_h, S(3));
    const Lib_Track *curt = current_track(app);
    for (u32 i = lv.first; i < lv.last; i++) {
        Core_String name;
        const u32 *tracks;
        u32 track_count;
        group_get(lib, kind, i, &name, &tracks, &track_count);
        if (!track_count) continue;
        f32 y = lv.y0 + (f32)i * row_h;
        vec2 rp = vec2_make(pos.x, y + S(1)), rs = vec2_make(size.x - S(8), row_h - S(2));
        u64 id = ui_idx(ui_id(artists ? "ar.row" : "ge.row"), i);
        vec2 kc = vec2_make(rp.x + rs.x - S(46), rp.y + rs.y * 0.5f);
        Ui_Interact kb = ui_interact(ui, ui_idx(id, 0x4B), vec2_make(kc.x - S(14), kc.y - S(16)), vec2_make(S(28), S(32)),
                                     PLATFORM_CURSOR_HAND);
        Ui_Interact it = ui_interact(ui, id, rp, rs, PLATFORM_CURSOR_HAND);
        b32 has_cur = curt && (artists ? curt->artist_index : curt->genre_index) == i;
        if (it.hover_t > 0.01f) core_draw_rect_rounded(app->r, rp, rs, S(9), UI_RGBA(255, 255, 255, 0.045f * it.hover_t));
        if (it.pressed) ui_ripple(ui, ui->in->mouse_pos, rs.x * 0.6f, UI_RGBA(160, 130, 255, 0.18f), rp, rs);
        const Lib_Track *t0 = track_at(app, tracks[0]);
        f32 th = S(50);
        if (t0) draw_thumb(app, t0, vec2_make(rp.x + S(13), rp.y + (rs.y - th) * 0.5f), th, artists ? th * 0.5f : S(6), 1);
        f32 tx = rp.x + S(86);
        ui_text(ui, ui->font, name, tx, rp.y + rs.y * 0.5f - S(9.5f), S(15),
                has_cur ? UI_LAVENDER : UI_RGBA(232, 229, 244, 1), UI_ALIGN_LEFT, rs.x - S(140));
        ui_text(ui, ui->font, str_fmt(app, "%u song%s", track_count, track_count == 1 ? "" : "s"),
                tx, rp.y + rs.y * 0.5f + S(11), S(13), UI_TEXT_DIM, UI_ALIGN_LEFT, 0);
        if (kb.hover_t > 0.01f) core_draw_circle(app->r, kc, S(14), UI_RGBA(255, 255, 255, 0.07f * kb.hover_t));
        ui_icon_kebab(ui, kc, S(14), ui_mix(UI_TEXT_DIM, UI_TEXT, kb.hover_t));
        ui_icon_chevron_right(ui, vec2_make(rp.x + rs.x - S(18) + S(3) * it.hover_t, rp.y + rs.y * 0.5f), S(20),
                              ui_mix(UI_TEXT_FAINT, UI_TEXT, it.hover_t));
        if (kb.clicked || it.right_clicked) {
            if (artists) open_artist_menu(app, i); else open_genre_menu(app, i);
        } else if (it.clicked) {
            *open = (s32)i;
            *open_scroll = (Scroll){0};
            ui_anim_set(ui, ui_id(artists ? "artist.slide" : "genre.slide"), 0);
        }
    }
    list_end(app, &lv);
    CORE_UNUSED(playing);
}

static void draw_panel(App *app, f32 x0, f32 w, f32 h, b32 playing) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    core_draw_rect(r, vec2_make(x0, 0), vec2_make(w, h), UI_PANEL);
    core_draw_rect(r, vec2_make(x0, 0), vec2_make(S(1), h), UI_RGBA(255, 255, 255, 0.07f));

    draw_window_buttons(app, x0 + w, S(20));
    draw_tab_bar(app, x0, S(69), w);

    vec2 lp = vec2_make(x0 + S(29), S(115));
    vec2 ls = vec2_make(w - S(29) - S(15), h - lp.y - S(6));
    /* fade lists in/out when switching tabs */
    if (!app->lib) { draw_skeleton_rows(app, lp, ls, S(58)); return; }
    switch (app->tab) {
        case TAB_QUEUE:
            draw_queue_list(app, lp, ls, playing);
            break;
        case TAB_GENRES:
            draw_groups(app, lp, ls, playing, GROUP_GENRE);
            break;
        case TAB_ARTISTS:
            draw_groups(app, lp, ls, playing, GROUP_ARTIST);
            break;
        default:
            draw_songs_list(app, lp, ls, app->lib->by_title, app->lib->track_count, &app->scroll[TAB_SONGS],
                            ui_id("list.songs"), playing);
            break;
    }
    /* soft fade at the list's top/bottom edges */
    core_draw_rect_gradient(r, vec2_make(x0 + S(1), h - S(24)), vec2_make(w - S(1), S(24)),
                            UI_RGBA(17, 15, 29, 0), UI_RGBA(15, 13, 26, 0.9f), false);
    CORE_UNUSED(ui);
}

/* ------------------------------------------------------------------------- */
/* command palette                                                           */
/* ------------------------------------------------------------------------- */

static void run_search(App *app) {
    u32 h = 2166136261u;
    for (u32 i = 0; i < app->query_len; i++) { h ^= app->query[i]; h *= 16777619u; }
    h ^= app->lib ? app->lib->track_count : 0;
    h |= 1;
    if (h == app->query_hash_done) return;
    app->query_hash_done = h;
    app->hit_count = search_tracks(app->lib, (Core_String){ .str = app->query, .len = app->query_len },
                                   app->hits, SEARCH_MAX);
    app->hit_sel = 0;
    app->hit_scroll = 0;
    ui_anim_set(&app->ui, ui_id("pal.results"), 0);
}

static void open_search(App *app, b32 open) {
    if (open == app->search_open) return;
    app->search_open = open;
    app->search_opened_at = app->now;
    app->menu = MENU_NONE;
    if (open) {
        app->query_hash_done = 0;
        run_search(app);
    }
}

static void search_play(App *app, u32 track, b32 enqueue_only) {
    if (enqueue_only) {
        queue_insert_next(app, track);
        return;
    }
    /* play now: insert after current and jump to it (queue stays intact) */
    queue_insert_next(app, track);
    play_index(app, app->cur + 1, 0, false);
}

/* Draw `s` with the codepoints flagged in `mask` (bit i = i-th codepoint)
   picked out: bright text on a soft accent pill. Returns the width drawn.
   `mask` 0 draws plain text. */
static f32 draw_marked(App *app, Core_String s, u64 mask, f32 x, f32 y, f32 px,
                       vec4 col, f32 alpha, f32 max_w) {
    Ui *ui = &app->ui;
    f32 x0 = x;
    if (!mask) return ui_text(ui, ui->font, s, x, y, px, ui_alpha(col, alpha), UI_ALIGN_LEFT, max_w);
    u32 idx = 0;
    for (u64 at = 0; at < s.len && (max_w <= 0 || x - x0 < max_w); ) {
        /* extend a run of equally-marked codepoints */
        u64 start = at;
        b32 marked = idx < 64 && ((mask >> idx) & 1);
        while (at < s.len) {
            b32 m = idx < 64 && ((mask >> idx) & 1);
            if (m != marked) break;
            at += core_utf8_decode(s, at).size;
            idx++;
        }
        Core_String run = core_str_substr(s, start, at - start);
        f32 room = max_w > 0 ? max_w - (x - x0) : 0;
        f32 w = ui_text_width(ui, ui->font, run, px);
        if (marked) {
            f32 pw = (room > 0 && w > room) ? room : w;
            core_draw_rect_rounded(app->r, vec2_make(x - S(2), y - px * 0.62f), vec2_make(pw + S(4), px * 1.24f),
                                   S(4), ui_alpha(UI_ACCENT, 0.30f * alpha));
        }
        w = ui_text(ui, ui->font, run, x, y, px, ui_alpha(marked ? UI_TEXT : col, alpha), UI_ALIGN_LEFT, room);
        x += w;
    }
    return x - x0;
}

/* Second line of a result: "Artist · Album", each part highlighted where the
   query matched it. Artist gets up to 55% of the width so the album always
   has room to show. */
static void draw_hit_meta(App *app, const Lib_Track *t, const Search_Hit *h,
                          f32 x, f32 y, f32 px, f32 alpha, f32 max_w) {
    Ui *ui = &app->ui;
    b32 has_album = t->album.len > 0;
    f32 artist_w = ui_text_width(ui, ui->font, t->artist, px);
    f32 artist_max = has_album ? max_w * 0.55f : max_w;
    if (artist_w > artist_max) artist_w = artist_max;
    draw_marked(app, t->artist, h->mask[SEARCH_ARTIST], x, y, px, UI_LAVENDER, alpha, artist_max);
    if (!has_album) return;
    f32 ax = x + artist_w;
    Core_String dot = core_str_lit("  ·  ");
    f32 dw = ui_text_width(ui, ui->font, dot, px);
    ui_text(ui, ui->font, dot, ax, y, px, ui_alpha(UI_TEXT_FAINT, alpha), UI_ALIGN_LEFT, 0);
    ax += dw;
    draw_marked(app, t->album, h->mask[SEARCH_ALBUM], ax, y, px, UI_TEXT_DIM, alpha, max_w - (ax - x));
}

/* the palette is 20% wider than its original 545 design width; heights are unchanged */
#define PALETTE_W 654 /* 545 * 1.2 */
#define PALETTE_IW (PALETTE_W - 54) /* content width inside the 29px side padding */

static void draw_palette(App *app, vec2 win) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    const Platform_Input *in = ui->in;
    f32 open = ui_spring(ui, ui_id("pal.open"), app->search_open ? 1.0f : 0.0f, 420, 34);
    if (open < 0.004f && !app->search_open) return;
    f32 vis = clamp01(open);

    /* backdrop */
    core_draw_rect(r, vec2_zero(), win, UI_RGBA(6, 5, 12, 0.42f * vis));

    if (app->search_open && app->menu == MENU_NONE) {
        /* keyboard (idle while a context menu floats above) */
        if (in->text_len) {
            u32 n = CORE_MIN(in->text_len, (u32)sizeof(app->query) - app->query_len);
            memcpy(app->query + app->query_len, in->text, n);
            app->query_len += n;
        }
        if (in->key_pressed[PLATFORM_KEY_BACKSPACE] && app->query_len) {
            if (in->ctrl) {
                while (app->query_len && app->query[app->query_len - 1] == ' ') app->query_len--;
                while (app->query_len && app->query[app->query_len - 1] != ' ') app->query_len--;
            } else {
                u32 n = app->query_len - 1;
                while (n > 0 && (app->query[n] & 0xC0) == 0x80) n--;
                app->query_len = n;
            }
        }
        run_search(app);
        if (in->key_pressed[PLATFORM_KEY_DOWN] || (in->key_pressed[PLATFORM_KEY_TAB] && !in->shift) ||
            (in->ctrl && in->key_pressed[PLATFORM_KEY_N]))
            app->hit_sel = app->hit_count ? (app->hit_sel + 1) % (s32)app->hit_count : 0;
        if (in->key_pressed[PLATFORM_KEY_UP] || (in->key_pressed[PLATFORM_KEY_TAB] && in->shift) ||
            (in->ctrl && in->key_pressed[PLATFORM_KEY_P]))
            app->hit_sel = app->hit_count ? (app->hit_sel + (s32)app->hit_count - 1) % (s32)app->hit_count : 0;
        if (in->key_pressed[PLATFORM_KEY_ENTER] && app->hit_count) {
            search_play(app, app->hits[app->hit_sel].track, in->shift);
            open_search(app, false);
        }
        if (in->key_pressed[PLATFORM_KEY_ESCAPE]) {
            if (app->query_len) { app->query_len = 0; run_search(app); }
            else open_search(app, false);
        }
    }

    f32 pw = S(PALETTE_W);
    u32 shown_rows = CORE_MIN(app->hit_count, 4u);
    f32 rows_h = app->hit_count ? S(67) * (f32)CORE_MAX(shown_rows, 1u) : S(64);
    f32 target_h = S(101) + rows_h + S(78) - S(10);
    f32 ph = ui_spring(ui, ui_id("pal.h"), target_h, 500, 40);
    f32 sc = 0.94f + 0.06f * open;
    vec2 center = vec2_make(win.x * 0.5f, win.y * 0.5f + S(3));
    f32 w2 = pw * sc, h2 = ph * sc;
    vec2 p = vec2_make(center.x - w2 * 0.5f, center.y - S(216) * sc - S(12) * (1 - open));
    ui->input_enabled = app->search_open && app->menu == MENU_NONE;
    if (ui->input_enabled && in->mouse_pressed[PLATFORM_MOUSE_LEFT] && !ui_mouse_in(ui, p, vec2_make(w2, h2)))
        open_search(app, false);

    core_draw_shadow(r, vec2_make(p.x, p.y + S(20)), vec2_make(w2, h2), S(14), S(50), UI_RGBA(0, 0, 0, 0.6f * vis));
    core_draw_shadow(r, p, vec2_make(w2, h2), S(14), S(26), UI_RGBA(110, 70, 255, 0.10f * vis));
    Core_BoxStyle bg = { .radius = S(14), .fill = UI_RGBA(24, 21, 38, 0.93f * vis),
                         .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.11f * vis) };
    core_draw_box(r, p, vec2_make(w2, h2), &bg);
    core_clip_push(r, p, vec2_make(w2, h2));

    f32 s = sc; /* inner layout scales with the pop-in */
    #define PX(v) (p.x + S(v) * s)
    #define PY(v) (p.y + S(v) * s)

    /* input */
    vec2 ip = vec2_make(PX(29), PY(30));
    vec2 is = vec2_make(S(PALETTE_IW) * s, S(53) * s);
    f32 glow = 0.6f + 0.4f * sinf((f32)app->now * 2.4f);
    core_set_blend(r, CORE_BLEND_ADD);
    core_draw_shadow(r, ip, is, S(9), S(10), UI_RGBA(139, 92, 246, (0.18f + 0.08f * glow) * vis));
    core_set_blend(r, CORE_BLEND_NORMAL);
    Core_BoxStyle ib = { .radius = S(9), .fill = UI_RGBA(20, 17, 34, 0.9f * vis),
                         .border = S(1.5f), .border_color = UI_RGBA(139, 102, 255, 0.95f * vis) };
    core_draw_box(r, ip, is, &ib);
    ui_icon_chevron_right(ui, vec2_make(PX(57), ip.y + is.y * 0.5f), S(22) * s, ui_alpha(UI_TEXT, vis));
    Core_String q = { .str = app->query, .len = app->query_len };
    f32 qx = PX(80);
    f32 qy = ip.y + is.y * 0.5f;
    f32 qw = 0;
    if (app->query_len) qw = ui_text(ui, ui->font_mono, q, qx, qy, S(19) * s, ui_alpha(UI_TEXT, vis), UI_ALIGN_LEFT, S(300));
    else ui_text(ui, ui->font, core_str_lit("Search songs, artists and albums"), qx + S(4), qy, S(17) * s, ui_alpha(UI_TEXT_FAINT, vis), UI_ALIGN_LEFT, 0);
    /* caret: solid while typing, blinks when idle */
    f32 blink = fmodf((f32)(app->now - app->search_opened_at), 1.06f) < 0.6f ? 1.0f : 0.25f;
    if (in->text_len || in->key_pressed[PLATFORM_KEY_BACKSPACE]) app->caret_blink = 0.5f;
    if (app->caret_blink > 0) { app->caret_blink -= ui->dt; blink = 1; }
    f32 cxp = ui_ease(ui, ui_id("pal.caret"), qx + qw + S(2), 30.0f);
    core_draw_rect(r, vec2_make(cxp, qy - S(12) * s), vec2_make(S(1.6f), S(24) * s), ui_alpha(UI_TEXT, vis * blink));
    if (app->search_open) ui->animating = true; /* caret */
    /* Ctrl K keycaps */
    f32 kx = ip.x + is.x - S(21) * s;
    kx -= ui_keycap(ui, core_str_lit("K"), kx, qy, S(26) * s, UI_ALIGN_RIGHT) + S(8) * s;
    ui_keycap(ui, core_str_lit("Ctrl"), kx, qy, S(26) * s, UI_ALIGN_RIGHT);

    /* results */
    f32 ry0 = PY(101);
    f32 row_h = S(67) * s;
    f32 appear = ui_ease(ui, ui_id("pal.results"), 1.0f, 7.0f);
    /* keep selection visible */
    if (app->hit_sel >= 0) {
        f32 sel_top = (f32)app->hit_sel;
        if (sel_top < app->hit_scroll) app->hit_scroll = sel_top;
        if (sel_top > app->hit_scroll + 3) app->hit_scroll = sel_top - 3;
    }
    if (ui_mouse_in(ui, vec2_make(p.x, ry0), vec2_make(w2, rows_h * s)) && in->scroll_y != 0 && app->hit_count > 4) {
        app->hit_scroll = CORE_CLAMP(app->hit_scroll - in->scroll_y, 0.0f, (f32)(app->hit_count - 4));
    }
    f32 scroll = ui_ease(ui, ui_id("pal.scroll"), app->hit_scroll, 18.0f);
    core_clip_push(r, vec2_make(p.x, ry0 - S(2)), vec2_make(w2, rows_h * s + S(2)));
    f32 sel_y = ui_spring(ui, ui_id("pal.sel"), ry0 + ((f32)app->hit_sel - scroll) * row_h, 600, 42);
    if (app->hit_count) {
        Core_BoxStyle sb = { .radius = S(9), .fill = UI_RGBA(255, 255, 255, 0.055f * vis),
                             .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.07f * vis) };
        core_draw_box(r, vec2_make(PX(29), sel_y), vec2_make(S(PALETTE_IW) * s, S(63) * s), &sb);
    }
    s32 first = (s32)floorf(scroll);
    for (s32 i = first; i < (s32)app->hit_count && i < first + 6; i++) {
        if (i < 0) continue;
        const Lib_Track *t = track_at(app, app->hits[i].track);
        if (!t) continue;
        f32 y = ry0 + ((f32)i - scroll) * row_h;
        f32 st = clamp01(appear * 3.0f - (f32)(i - first) * 0.35f);
        f32 e = ease_out_cubic(st);
        vec2 rp = vec2_make(PX(29), y), rs = vec2_make(S(PALETTE_IW) * s, S(63) * s);
        u64 id = ui_idx(ui_id("pal.row"), t->path_hash);
        Ui_Interact it = ui_interact(ui, id, rp, rs, PLATFORM_CURSOR_HAND);
        if (it.hovered && (in->mouse_delta.x != 0 || in->mouse_delta.y != 0)) app->hit_sel = i;
        if (it.clicked) { search_play(app, app->hits[i].track, in->shift); open_search(app, false); }
        if (it.right_clicked) { app->hit_sel = i; open_menu(app, app->hits[i].track, -1, true); }
        f32 ox = S(14) * (1 - e);
        draw_thumb(app, t, vec2_make(rp.x + S(16) * s + ox, y + (rs.y - S(50) * s) * 0.5f), S(50) * s, S(6), e * vis);
        f32 tx = rp.x + S(93) * s + ox;
        draw_marked(app, t->title, app->hits[i].mask[SEARCH_TITLE], tx, y + S(21) * s, S(16) * s, UI_TEXT, e * vis, S(330 + PALETTE_W - 545));
        draw_hit_meta(app, t, &app->hits[i], tx, y + S(45) * s, S(14) * s, e * vis, S(330 + PALETTE_W - 545));
        if (i == app->hit_sel) {
            vec2 kc = vec2_make(rp.x + rs.x - S(36) * s, y + rs.y * 0.5f);
            Core_BoxStyle kb = { .radius = S(7), .fill = UI_RGBA(139, 102, 255, 0.22f * vis),
                                 .border = S(1), .border_color = UI_RGBA(160, 130, 255, 0.35f * vis) };
            core_draw_box(r, vec2_make(kc.x - S(18) * s, kc.y - S(18) * s), vec2_make(S(36) * s, S(36) * s), &kb);
            ui_icon_enter(ui, kc, S(18) * s, ui_alpha(UI_ACCENT_BRIGHT, vis));
        }
    }
    if (!app->hit_count) {
        Core_String msg = app->query_len ? core_str_lit("No matches") :
            str_fmt(app, "Type to search %u songs", app->lib ? app->lib->track_count : 0);
        ui_text(ui, ui->font, msg, p.x + w2 * 0.5f, ry0 + S(30) * s, S(15) * s, ui_alpha(UI_TEXT_DIM, vis), UI_ALIGN_CENTER, 0);
    }
    core_clip_pop(r);

    /* footer */
    f32 fy = ry0 + rows_h * s + S(12) * s;
    core_draw_rect(r, vec2_make(PX(29), fy), vec2_make(S(PALETTE_IW) * s, S(1)), UI_RGBA(255, 255, 255, 0.08f * vis));
    f32 cy = fy + S(25) * s;
    f32 x = PX(PALETTE_W - 38);
    Core_String t2 = core_str_lit("to close");
    x -= ui_text_width(ui, ui->font, t2, S(14) * s);
    ui_text(ui, ui->font, t2, x, cy, S(14) * s, ui_alpha(UI_TEXT_DIM, vis), UI_ALIGN_LEFT, 0);
    x -= S(11) * s;
    x -= ui_keycap(ui, core_str_lit("Esc"), x, cy, S(24) * s, UI_ALIGN_RIGHT) + S(26) * s;
    Core_String t1 = core_str_lit("to play");
    x -= ui_text_width(ui, ui->font, t1, S(14) * s);
    ui_text(ui, ui->font, t1, x, cy, S(14) * s, ui_alpha(UI_TEXT_DIM, vis), UI_ALIGN_LEFT, 0);
    x -= S(11) * s;
    ui_keycap(ui, core_str_lit("Enter"), x, cy, S(24) * s, UI_ALIGN_RIGHT);
    #undef PX
    #undef PY

    core_clip_pop(r);
    /* after the rows: blocking first would swallow the press they need */
    ui_block(ui, p, vec2_make(w2, h2));
    ui->input_enabled = true;
}

/* ------------------------------------------------------------------------- */
/* context menu                                                              */
/* ------------------------------------------------------------------------- */

enum { ACT_NEXT, ACT_QUEUE, ACT_REMOVE, ACT_ARTIST_NEXT, ACT_ARTIST_QUEUE, ACT_GENRE_NEXT, ACT_GENRE_QUEUE };

static void draw_menu(App *app, vec2 win) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    if (app->menu == MENU_NONE) return;
    f32 t = ui_spring(ui, ui_id("menu.t"), 1.0f, 520, 32);
    static const char *const labels[] = {
        [ACT_NEXT] = "Play next", [ACT_QUEUE] = "Add to queue", [ACT_REMOVE] = "Remove from queue",
        [ACT_ARTIST_NEXT] = "Play artist next", [ACT_ARTIST_QUEUE] = "Queue all from artist",
        [ACT_GENRE_NEXT] = "Play genre next", [ACT_GENRE_QUEUE] = "Queue all from genre",
    };
    u8 acts[4];
    u32 n = 0;
    acts[n++] = app->menu == MENU_ARTIST ? ACT_ARTIST_NEXT : app->menu == MENU_GENRE ? ACT_GENRE_NEXT : ACT_NEXT;
    acts[n++] = app->menu == MENU_ARTIST ? ACT_ARTIST_QUEUE : app->menu == MENU_GENRE ? ACT_GENRE_QUEUE : ACT_QUEUE;
    if (app->menu == MENU_TRACK && app->menu_queue_index >= 0) acts[n++] = ACT_REMOVE;
    if (app->menu == MENU_TRACK && app->menu_artist_actions) { acts[n++] = ACT_ARTIST_QUEUE; acts[n++] = ACT_ARTIST_NEXT; }
    f32 w = S(214), ih = S(38);
    f32 h = ih * (f32)n + S(12);
    vec2 p = app->menu_pos;
    if (p.x + w > win.x - S(8)) p.x = win.x - S(8) - w;
    if (p.y + h > win.y - S(8)) p.y = win.y - S(8) - h;
    f32 sc = 0.9f + 0.1f * t;
    vec2 sz = vec2_make(w * sc, h * sc);
    ui->input_enabled = true;
    core_draw_shadow(r, vec2_make(p.x, p.y + S(8)), sz, S(10), S(24), UI_RGBA(0, 0, 0, 0.5f * clamp01(t)));
    Core_BoxStyle bg = { .radius = S(10), .fill = UI_RGBA(30, 26, 48, 0.97f * clamp01(t)),
                         .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.1f * clamp01(t)) };
    core_draw_box(r, p, sz, &bg);
    core_clip_push(r, p, sz);
    for (u32 i = 0; i < n; i++) {
        u32 act = acts[i];
        vec2 ip = vec2_make(p.x + S(6), p.y + S(6) + ih * (f32)i * sc);
        vec2 is = vec2_make(sz.x - S(12), ih * sc);
        Ui_Interact it = ui_interact(ui, ui_idx(ui_id("menu.item"), i), ip, is, PLATFORM_CURSOR_HAND);
        if (it.hover_t > 0.01f) core_draw_rect_rounded(r, ip, is, S(7), UI_RGBA(139, 102, 255, 0.22f * it.hover_t));
        vec2 ic = vec2_make(ip.x + S(18), ip.y + is.y * 0.5f);
        vec4 col = ui_alpha(ui_mix(UI_RGBA(215, 210, 235, 1), UI_TEXT, it.hover_t), clamp01(t));
        if (act == ACT_NEXT || act == ACT_ARTIST_NEXT || act == ACT_GENRE_NEXT) ui_icon_queue_next(ui, ic, S(16), col);
        else if (act == ACT_QUEUE || act == ACT_ARTIST_QUEUE || act == ACT_GENRE_QUEUE) ui_icon_plus(ui, ic, S(16), col);
        else ui_icon_minus(ui, ic, S(16), col);
        ui_text(ui, ui->font, core_str(labels[act]), ip.x + S(38), ic.y, S(14), col, UI_ALIGN_LEFT, 0);
        if (it.clicked) {
            const Lib_Artist *a = 0;
            const Lib_Genre *g = 0;
            if (app->lib && act >= ACT_GENRE_NEXT) {
                if (app->menu_genre < app->lib->genre_count) g = &app->lib->genres[app->menu_genre];
            } else if (app->lib && act >= ACT_ARTIST_NEXT) {
                u32 ai = app->menu == MENU_ARTIST ? app->menu_artist : UINT32_MAX;
                if (app->menu == MENU_TRACK) {
                    const Lib_Track *mt = track_at(app, app->menu_track);
                    if (mt) ai = mt->artist_index;
                }
                if (ai < app->lib->artist_count) a = &app->lib->artists[ai];
            }
            switch (act) {
            case ACT_NEXT:         queue_insert_next(app, app->menu_track); break;
            case ACT_QUEUE:        queue_append(app, app->menu_track); break;
            case ACT_REMOVE:       queue_remove(app, (u32)app->menu_queue_index); break;
            case ACT_ARTIST_NEXT:  if (a) queue_insert_next_many(app, a->tracks, a->track_count); break;
            case ACT_ARTIST_QUEUE: if (a) queue_append_many(app, a->tracks, a->track_count); break;
            case ACT_GENRE_NEXT:   if (g) queue_insert_next_many(app, g->tracks, g->track_count); break;
            case ACT_GENRE_QUEUE:  if (g) queue_append_many(app, g->tracks, g->track_count); break;
            }
            app->menu = MENU_NONE;
        }
    }
    core_clip_pop(r);
    ui_block(ui, p, sz);
    if ((ui->in->mouse_pressed[PLATFORM_MOUSE_LEFT] || ui->in->mouse_pressed[PLATFORM_MOUSE_RIGHT]) &&
        !ui_mouse_in(ui, p, sz))
        app->menu = MENU_NONE;
    if (ui->in->key_pressed[PLATFORM_KEY_ESCAPE]) app->menu = MENU_NONE;
}

/* ------------------------------------------------------------------------- */
/* settings modal                                                            */
/* ------------------------------------------------------------------------- */

/* Main page: sections of rows (label + hint on the left, control on the
   right, or a control strip below). New settings are a new row in
   draw_settings_main plus a field in settings.h. The "browse" page replaces
   the rows with an in-app folder picker for the music folder. */

static void browse_to(App *app, const char *path) {
    char norm[sizeof(app->browse_path)];
    if (strlen(path) >= sizeof(norm)) return; /* absurdly deep: stay put */
    snprintf(norm, sizeof(norm), "%s", path[0] ? path : "/");
    u64 n = strlen(norm);
    while (n > 1 && norm[n - 1] == '/') norm[--n] = 0;
    memcpy(app->browse_path, norm, n + 1);
    core_arena_reset(&app->browse_arena);
    app->browse_count = platform_list_dirs(&app->browse_arena, app->browse_path, &app->browse_names);
    app->browse_scroll = (Scroll){0};
    ui_anim_set(&app->ui, ui_id("set.browse.appear"), 0);
}

static void browse_up(App *app) {
    char *slash = strrchr(app->browse_path, '/');
    if (!slash || app->browse_path[1] == 0) return;
    char parent[1024];
    u64 n = slash == app->browse_path ? 1 : (u64)(slash - app->browse_path);
    memcpy(parent, app->browse_path, n);
    parent[n] = 0;
    browse_to(app, parent);
}

static void open_browse(App *app, b32 open) {
    app->browse_open = open;
    if (open) {
        /* start where the library is; fall back to home if it's gone */
        const char *start = platform_file_info(app->music_dir).is_dir ? app->music_dir : platform_home_dir(&app->frame);
        browse_to(app, start);
    }
}

static void open_settings(App *app, b32 open) {
    if (open == app->settings_open) return;
    app->settings_open = open;
    app->menu = MENU_NONE;
    app->browse_open = false;
    if (open) ui_anim_set(&app->ui, ui_id("set.page"), 0);
}

/* Pill button; `primary` is filled with the accent. Returns the interaction. */
static Ui_Interact settings_button(App *app, const char *name, Core_String label, vec2 pos, vec2 size,
                                   b32 primary, f32 vis) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    Ui_Interact it = ui_interact(ui, ui_id(name), pos, size, PLATFORM_CURSOR_HAND);
    f32 rad = size.y * 0.5f;
    if (primary) {
        Core_BoxStyle st = { .radius = rad, .fill = ui_alpha(ui_mix(UI_RGBA(128, 98, 236, 1), UI_RGBA(146, 116, 250, 1), it.hover_t), vis),
                             .fill2 = UI_RGBA(100, 72, 210, vis), .gradient = 1 };
        core_draw_box(r, pos, size, &st);
    } else {
        Core_BoxStyle st = { .radius = rad, .fill = UI_RGBA(255, 255, 255, (0.05f + 0.06f * it.hover_t) * vis),
                             .border = S(1), .border_color = UI_RGBA(255, 255, 255, (0.12f + 0.08f * it.hover_t) * vis) };
        core_draw_box(r, pos, size, &st);
    }
    if (it.pressed)
        ui_ripple(ui, ui->in->mouse_pos, size.x, UI_RGBA(190, 160, 255, 0.3f), pos, size);
    f32 px = S(14) * (1 - 0.03f * it.press_t);
    ui_text(ui, ui->font_med, label, pos.x + size.x * 0.5f, pos.y + size.y * 0.5f, px,
            ui_alpha(primary ? UI_TEXT : ui_mix(UI_RGBA(215, 210, 235, 1), UI_TEXT, it.hover_t), vis), UI_ALIGN_CENTER, 0);
    return it;
}

static f32 settings_button_w(App *app, Core_String label) {
    Ui *ui = &app->ui;
    return ui_text_width(ui, ui->font_med, label, S(14)) + S(36);
}

/* Section heading: small faint caps with a hairline. Returns the next y. */
static f32 settings_section(App *app, const char *title, f32 x, f32 y, f32 w, f32 vis) {
    Ui *ui = &app->ui;
    f32 tw = ui_text(ui, ui->font_semi, core_str(title), x, y, S(11.5f), ui_alpha(UI_TEXT_FAINT, vis), UI_ALIGN_LEFT, 0);
    core_draw_rect(app->r, vec2_make(x + tw + S(12), y), vec2_make(w - tw - S(12), S(1)), UI_RGBA(255, 255, 255, 0.07f * vis));
    return y + S(28);
}

/* Row label with an optional hint line under it. */
static void settings_label(App *app, const char *label, Core_String hint, f32 x, f32 y, f32 max_w, f32 vis) {
    Ui *ui = &app->ui;
    f32 ly = hint.len ? y - S(10) : y;
    ui_text(ui, ui->font_med, core_str(label), x, ly, S(15.5f), ui_alpha(UI_TEXT, vis), UI_ALIGN_LEFT, max_w);
    if (hint.len) ui_text(ui, ui->font, hint, x, y + S(11), S(13), ui_alpha(UI_TEXT_DIM, vis), UI_ALIGN_LEFT, max_w);
}

static b32 settings_toggle(App *app, const char *name, vec2 pos, b32 on, f32 vis) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    vec2 size = vec2_make(S(44), S(26));
    u64 id = ui_id(name);
    Ui_Interact it = ui_interact(ui, id, vec2_make(pos.x - S(4), pos.y - S(4)), vec2_make(size.x + S(8), size.y + S(8)),
                                 PLATFORM_CURSOR_HAND);
    if (it.clicked) on = !on;
    f32 t = ui_spring(ui, ui_idx(id, 1), on ? 1.0f : 0.0f, 420, 26);
    f32 tc = clamp01(t);
    Core_BoxStyle track = { .radius = size.y * 0.5f,
                            .fill = ui_alpha(ui_mix(UI_RGBA(255, 255, 255, 0.10f + 0.04f * it.hover_t), UI_RGBA(128, 98, 236, 1), tc), vis),
                            .border = S(1), .border_color = UI_RGBA(255, 255, 255, (0.10f + 0.1f * (1 - tc)) * vis) };
    core_draw_box(r, pos, size, &track);
    f32 kr = S(9) * (1 + 0.08f * it.press_t);
    vec2 kc = vec2_make(pos.x + S(13) + (size.x - S(26)) * t, pos.y + size.y * 0.5f);
    core_draw_shadow(r, vec2_make(kc.x - kr, kc.y - kr + S(1)), vec2_make(kr * 2, kr * 2), kr, S(4), UI_RGBA(0, 0, 0, 0.35f * vis));
    core_draw_circle(r, kc, kr, UI_RGBA(255, 255, 255, vis));
    return on;
}

/* Segmented control with a sliding highlight. Returns the selection. */
static u32 settings_segmented(App *app, const char *name, const char *const *labels, u32 count, u32 sel,
                              vec2 pos, f32 seg_w, f32 vis) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    f32 h = S(34);
    u64 id = ui_id(name);
    Core_BoxStyle bg = { .radius = S(10), .fill = UI_RGBA(0, 0, 0, 0.22f * vis),
                         .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.08f * vis) };
    core_draw_box(r, pos, vec2_make(seg_w * (f32)count + S(6), h + S(6)), &bg);
    f32 hx = ui_spring(ui, ui_idx(id, 0xFF), pos.x + S(3) + seg_w * (f32)sel, 480, 34);
    Core_BoxStyle hl = { .radius = S(8), .fill = UI_RGBA(139, 102, 255, 0.30f * vis),
                         .border = S(1), .border_color = UI_RGBA(160, 130, 255, 0.55f * vis) };
    core_draw_box(r, vec2_make(hx, pos.y + S(3)), vec2_make(seg_w, h), &hl);
    for (u32 i = 0; i < count; i++) {
        vec2 sp = vec2_make(pos.x + S(3) + seg_w * (f32)i, pos.y + S(3));
        Ui_Interact it = ui_interact(ui, ui_idx(id, i), sp, vec2_make(seg_w, h), PLATFORM_CURSOR_HAND);
        if (it.clicked) sel = i;
        f32 act = ui_ease(ui, ui_idx(id, 0x100 + i), i == sel ? 1.0f : 0.0f, 14.0f);
        vec4 col = ui_mix(ui_mix(UI_TEXT_DIM, UI_RGBA(210, 206, 228, 1), it.hover_t), UI_TEXT, act);
        ui_text(ui, ui->font_med, core_str(labels[i]), sp.x + seg_w * 0.5f, sp.y + h * 0.5f, S(14),
                ui_alpha(col, vis), UI_ALIGN_CENTER, 0);
    }
    return sel;
}

/* Theme swatches, each drawn through its own theme's color matrix so it
   previews exactly what picking it does. */
static u32 settings_themes(App *app, vec2 pos, f32 w, u32 sel, f32 vis) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    u32 n = SETTINGS_THEME_COUNT;
    f32 cell = w / (f32)n;
    f32 sw = S(46);
    f32 m[9];
    for (u32 i = 0; i < n; i++) {
        vec2 c = vec2_make(pos.x + cell * ((f32)i + 0.5f), pos.y + sw * 0.5f);
        u64 id = ui_idx(ui_id("set.theme"), i);
        Ui_Interact it = ui_interact(ui, id, vec2_make(c.x - cell * 0.5f, pos.y - S(4)), vec2_make(cell, sw + S(30)),
                                     PLATFORM_CURSOR_HAND);
        if (it.clicked) {
            sel = i;
            ui_anim_kick(ui, ui_idx(id, 9), 10.0f);
        }
        f32 act = ui_ease(ui, ui_idx(id, 1), i == sel ? 1.0f : 0.0f, 12.0f);
        f32 pop = ui_spring(ui, ui_idx(id, 9), 0, 320, 14);
        f32 s = sw * (1 + 0.05f * it.hover_t + 0.04f * pop - 0.05f * it.press_t);
        settings_theme_matrix(i, m);
        core_set_color_matrix(r, m);
        if (act > 0.01f) {
            core_set_blend(r, CORE_BLEND_ADD);
            core_draw_circle_ex(r, c, s * 0.62f, 0, s * 0.5f, UI_RGBA(139, 92, 246, 0.35f * act * vis));
            core_set_blend(r, CORE_BLEND_NORMAL);
            core_draw_circle_ex(r, c, s * 0.5f + S(4), S(1.8f), 0, UI_RGBA(171, 140, 255, act * vis));
        }
        Core_BoxStyle disc = { .radius = s * 0.5f, .fill = UI_RGBA(186, 150, 255, vis),
                               .fill2 = UI_RGBA(92, 50, 210, vis), .gradient = 1 };
        core_draw_box(r, vec2_make(c.x - s * 0.5f, c.y - s * 0.5f), vec2_make(s, s), &disc);
        /* warm crescent of the ribbon's pink so hue shifts read at a glance */
        core_set_blend(r, CORE_BLEND_ADD);
        core_draw_circle_ex(r, vec2_make(c.x - s * 0.16f, c.y + s * 0.18f), s * 0.22f, 0, s * 0.2f, UI_RGBA(255, 72, 140, 0.45f * vis));
        core_set_blend(r, CORE_BLEND_NORMAL);
        core_set_color_matrix(r, 0);
        vec4 lc = ui_mix(ui_mix(UI_TEXT_DIM, UI_RGBA(210, 206, 228, 1), it.hover_t), UI_TEXT, act);
        ui_text(ui, ui->font, core_str(SETTINGS_THEMES[i].label), c.x, pos.y + sw + S(16), S(13),
                ui_alpha(lc, vis), UI_ALIGN_CENTER, cell);
    }
    core_set_color_matrix(r, app->theme_m); /* back to the active theme */
    return sel;
}

/* Show the tail of a long path ("…/Music/Albums") within `max_w`. */
static void draw_path(App *app, Core_String path, f32 x, f32 y, f32 px, vec4 col, f32 max_w) {
    Ui *ui = &app->ui;
    Core_Font f = ui->font;
    if (ui_text_width(ui, f, path, px) > max_w) {
        Core_String ell = core_str_lit("…");
        f32 room = max_w - ui_text_width(ui, f, ell, px);
        u64 at = 0;
        while (at < path.len && ui_text_width(ui, f, core_str_substr(path, at, path.len - at), px) > room) {
            at++;
            while (at < path.len && (path.str[at] & 0xC0) == 0x80) at++;
        }
        x += ui_text(ui, f, ell, x, y, px, col, UI_ALIGN_LEFT, 0);
        path = core_str_substr(path, at, path.len - at);
    }
    ui_text(ui, f, path, x, y, px, col, UI_ALIGN_LEFT, 0);
}

static void draw_settings_main(App *app, vec2 p, f32 w, f32 vis) {
    Ui *ui = &app->ui;
    f32 x = p.x + S(32), cw = w - S(64), xr = x + cw;
    f32 y = p.y + S(92);

    /* ---- library ---- */
    y = settings_section(app, "LIBRARY", x, y, cw, vis);
    Core_String change = core_str_lit("Change"), rescan = core_str_lit("Rescan");
    f32 bw1 = settings_button_w(app, change), bw2 = settings_button_w(app, rescan), bh = S(34);
    f32 ry = y + S(16);
    ui_icon_folder(ui, vec2_make(x + S(14), ry), S(26), ui_alpha(UI_LAVENDER, vis));
    f32 lx = x + S(40), lw = cw - S(40) - bw1 - bw2 - S(26);
    ui_text(ui, ui->font_med, core_str_lit("Music folder"), lx, ry - S(10), S(15.5f), ui_alpha(UI_TEXT, vis), UI_ALIGN_LEFT, lw);
    draw_path(app, core_str(app->music_dir), lx, ry + S(11), S(13), ui_alpha(UI_TEXT_DIM, vis), lw);
    if (settings_button(app, "set.change", change, vec2_make(xr - bw1, ry - bh * 0.5f), vec2_make(bw1, bh), true, vis).clicked)
        open_browse(app, true);
    if (settings_button(app, "set.rescan", rescan, vec2_make(xr - bw1 - S(10) - bw2, ry - bh * 0.5f), vec2_make(bw2, bh), false, vis).clicked)
        rescan_library(app);
    Core_String status = app->scanner
        ? str_fmt(app, "Scanning, %u files seen", library_scanner_files_seen(app->scanner))
        : str_fmt(app, "%u song%s, %u artist%s", app->lib ? app->lib->track_count : 0, app->lib && app->lib->track_count == 1 ? "" : "s",
                  app->lib ? app->lib->artist_count : 0, app->lib && app->lib->artist_count == 1 ? "" : "s");
    ui_text(ui, ui->font, status, lx, ry + S(34), S(12.5f), ui_alpha(UI_TEXT_FAINT, vis), UI_ALIGN_LEFT, lw);
    if (app->scanner) ui->animating = true;
    y = ry + S(74);

    /* ---- appearance ---- */
    y = settings_section(app, "APPEARANCE", x, y, cw, vis);
    settings_label(app, "Theme", (Core_String){0}, x, y + S(4), cw, vis);
    u32 theme = settings_themes(app, vec2_make(x - S(6), y + S(26)), cw + S(12), app->settings.theme, vis);
    if (theme != app->settings.theme) { app->settings.theme = theme; save_settings(app); }
    y += S(122);
    f32 seg_w = S(84);
    f32 seg_x = xr - seg_w * VIS_COUNT - S(6);
    settings_label(app, "Visualizer", core_str_lit("Drawn around the cover"), x, y + S(20), seg_x - x - S(12), vis);
    u32 v = settings_segmented(app, "set.vis", SETTINGS_VIS_LABELS, VIS_COUNT, app->settings.vis,
                               vec2_make(seg_x, y), seg_w, vis);
    if (v != app->settings.vis) { app->settings.vis = v; save_settings(app); }
    y += S(76);

    /* ---- developer ---- */
    y = settings_section(app, "DEVELOPER", x, y, cw, vis);
    f32 dy = y + S(16);
    settings_label(app, "Debug overlay", core_str_lit("Frame time, memory and draw stats"), x, dy, cw - S(140), vis);
    b32 dbg = settings_toggle(app, "set.debug", vec2_make(xr - S(44), dy - S(13)), app->settings.debug, vis);
    if (dbg != app->settings.debug) { app->settings.debug = dbg; save_settings(app); }
    ui_keycap(ui, core_str_lit("F1"), xr - S(58), dy, S(24), UI_ALIGN_RIGHT);
}

static void draw_settings_browse(App *app, vec2 p, f32 w, f32 h, f32 vis) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    const Platform_Input *in = ui->in;
    f32 x = p.x + S(24), cw = w - S(48);

    /* current folder, with an "up" button */
    f32 by = p.y + S(84);
    vec2 bp = vec2_make(x, by), bs = vec2_make(cw, S(44));
    Core_BoxStyle bar = { .radius = S(10), .fill = UI_RGBA(0, 0, 0, 0.22f * vis),
                          .border = S(1), .border_color = UI_RGBA(255, 255, 255, 0.08f * vis) };
    core_draw_box(r, bp, bs, &bar);
    vec2 uc = vec2_make(bp.x + S(24), by + bs.y * 0.5f);
    b32 at_root = app->browse_path[1] == 0;
    Ui_Interact up = ui_interact(ui, ui_id("set.up"), vec2_make(uc.x - S(16), uc.y - S(16)), vec2_make(S(32), S(32)),
                                 at_root ? PLATFORM_CURSOR_DEFAULT : PLATFORM_CURSOR_HAND);
    if (up.hover_t > 0.01f && !at_root) core_draw_circle(r, uc, S(15), UI_RGBA(255, 255, 255, 0.08f * up.hover_t * vis));
    ui_icon_chevron_left(ui, uc, S(22), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, up.hover_t), vis * (at_root ? 0.35f : 1.0f)));
    if (up.clicked) browse_up(app);
    core_draw_rect(r, vec2_make(uc.x + S(22), by + S(10)), vec2_make(S(1), bs.y - S(20)), UI_RGBA(255, 255, 255, 0.08f * vis));
    draw_path(app, core_str(app->browse_path), uc.x + S(36), by + bs.y * 0.5f, S(14.5f), ui_alpha(UI_TEXT, vis),
              bs.x - S(76));

    /* subfolders */
    f32 row_h = S(46);
    vec2 lp = vec2_make(x, by + bs.y + S(12));
    vec2 ls = vec2_make(cw, p.y + h - S(80) - lp.y);
    f32 appear = ui_ease(ui, ui_id("set.browse.appear"), 1.0f, 8.0f);
    List_View lv = list_begin(app, &app->browse_scroll, ui_id("set.browse"), lp, ls, app->browse_count, row_h, 0);
    for (u32 i = lv.first; i < lv.last; i++) {
        f32 y = lv.y0 + row_h * (f32)i;
        f32 e = ease_out_cubic(clamp01(appear * 2.5f - (f32)(i - lv.first) * 0.12f));
        vec2 rp = vec2_make(lp.x, y), rs = vec2_make(ls.x - S(10), row_h - S(4));
        Ui_Interact it = ui_interact(ui, ui_idx(ui_id("set.dir"), i), rp, rs, PLATFORM_CURSOR_HAND);
        if (it.hover_t > 0.01f) core_draw_rect_rounded(r, rp, rs, S(8), UI_RGBA(139, 102, 255, 0.16f * it.hover_t * vis));
        f32 ox = S(10) * (1 - e);
        vec4 col = ui_alpha(ui_mix(UI_RGBA(215, 210, 235, 1), UI_TEXT, it.hover_t), e * vis);
        ui_icon_folder(ui, vec2_make(rp.x + S(22) + ox, rp.y + rs.y * 0.5f + S(1)), S(20), ui_alpha(UI_LAVENDER, e * vis));
        ui_text(ui, ui->font, core_str(app->browse_names[i]), rp.x + S(46) + ox, rp.y + rs.y * 0.5f, S(15), col,
                UI_ALIGN_LEFT, rs.x - S(90));
        ui_icon_chevron_right(ui, vec2_make(rp.x + rs.x - S(20), rp.y + rs.y * 0.5f), S(16),
                              ui_alpha(UI_TEXT_FAINT, e * vis * (0.5f + 0.5f * it.hover_t)));
        if (it.clicked) {
            char next[2048];
            snprintf(next, sizeof(next), "%s/%s", at_root ? "" : app->browse_path, app->browse_names[i]);
            browse_to(app, next);
            break; /* names were just replaced */
        }
    }
    if (!app->browse_count)
        ui_text(ui, ui->font, core_str_lit("No folders in here"), lp.x + ls.x * 0.5f, lp.y + S(40), S(14),
                ui_alpha(UI_TEXT_DIM, vis), UI_ALIGN_CENTER, 0);
    list_end(app, &lv);

    /* footer */
    f32 fy = p.y + h - S(40);
    core_draw_rect(r, vec2_make(x, fy - S(28)), vec2_make(cw, S(1)), UI_RGBA(255, 255, 255, 0.07f * vis));
    Core_String use = core_str_lit("Use this folder"), cancel = core_str_lit("Cancel");
    f32 bw1 = settings_button_w(app, use), bw2 = settings_button_w(app, cancel), bh = S(36);
    b32 chosen = settings_button(app, "set.use", use, vec2_make(x + cw - bw1, fy - bh * 0.5f), vec2_make(bw1, bh), true, vis).clicked;
    b32 back = settings_button(app, "set.cancel", cancel, vec2_make(x + cw - bw1 - S(10) - bw2, fy - bh * 0.5f),
                               vec2_make(bw2, bh), false, vis).clicked;
    ui_text(ui, ui->font, str_fmt(app, "%u folder%s", app->browse_count, app->browse_count == 1 ? "" : "s"),
            x, fy, S(13), ui_alpha(UI_TEXT_FAINT, vis), UI_ALIGN_LEFT, 0);

    if (ui->input_enabled && in->key_pressed[PLATFORM_KEY_BACKSPACE]) browse_up(app);
    if (ui->input_enabled && in->key_pressed[PLATFORM_KEY_ENTER]) chosen = true;
    if (chosen) {
        set_music_dir(app, app->browse_path);
        back = true;
    }
    if (back) app->browse_open = false;
}

static void draw_settings(App *app, vec2 win) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    const Platform_Input *in = ui->in;
    f32 open = ui_spring(ui, ui_id("set.open"), app->settings_open ? 1.0f : 0.0f, 420, 34);
    if (open < 0.004f && !app->settings_open) return;
    f32 vis = clamp01(open);
    b32 live = app->settings_open && app->menu == MENU_NONE;

    core_draw_rect(r, vec2_zero(), win, UI_RGBA(6, 5, 12, 0.5f * vis));

    /* page switch: the card resizes and the pages crossfade with a slide */
    f32 page = ui_spring(ui, ui_id("set.page"), app->browse_open ? 1.0f : 0.0f, 380, 32);
    f32 w = S(600);
    f32 h = ui_spring(ui, ui_id("set.h"), app->browse_open ? CORE_MIN(S(640), win.y - S(60)) : S(540), 480, 40);
    f32 sc = 0.94f + 0.06f * open;
    f32 w2 = w * sc, h2 = h * sc;
    vec2 p = vec2_make(roundf(win.x * 0.5f - w2 * 0.5f), roundf(win.y * 0.5f - h2 * 0.5f + S(14) * (1 - open)));

    ui->input_enabled = live;
    if (live && in->mouse_pressed[PLATFORM_MOUSE_LEFT] && !ui_mouse_in(ui, p, vec2_make(w2, h2)))
        open_settings(app, false);

    core_draw_shadow(r, vec2_make(p.x, p.y + S(22)), vec2_make(w2, h2), S(18), S(56), UI_RGBA(0, 0, 0, 0.6f * vis));
    core_draw_shadow(r, p, vec2_make(w2, h2), S(18), S(28), UI_RGBA(110, 70, 255, 0.10f * vis));
    Core_BoxStyle bg = { .radius = S(18), .fill = UI_RGBA(24, 21, 38, 0.95f * vis), .fill2 = UI_RGBA(19, 17, 31, 0.96f * vis),
                         .gradient = 1 };
    core_draw_box(r, p, vec2_make(w2, h2), &bg);
    Core_BoxStyle edge = { .radius = S(18), .fill = UI_RGBA(0, 0, 0, 0), .border = S(1),
                           .border_color = UI_RGBA(255, 255, 255, 0.11f * vis) };
    core_draw_box(r, p, vec2_make(w2, h2), &edge);
    core_clip_push(r, p, vec2_make(w2, h2));

    /* header: title slides between "Settings" and the picker's title */
    f32 hy = p.y + S(44);
    f32 pg = clamp01(page);
    ui_icon_gear(ui, vec2_make(p.x + S(46) - S(30) * pg, hy), S(24), ui_alpha(UI_LAVENDER, vis * (1 - pg)));
    ui_text(ui, ui->font_semi, core_str_lit("Settings"), p.x + S(70) - S(30) * pg, hy, S(22),
            ui_alpha(UI_TEXT, vis * (1 - pg)), UI_ALIGN_LEFT, 0);
    ui_text(ui, ui->font_semi, core_str_lit("Choose music folder"), p.x + S(32) + S(30) * (1 - pg), hy, S(22),
            ui_alpha(UI_TEXT, vis * pg), UI_ALIGN_LEFT, 0);
    vec2 xc = vec2_make(p.x + w2 - S(40), hy);
    Ui_Interact close = ui_interact(ui, ui_id("set.close"), vec2_make(xc.x - S(17), xc.y - S(17)), vec2_make(S(34), S(34)),
                                    PLATFORM_CURSOR_HAND);
    if (close.hover_t > 0.01f) core_draw_circle(r, xc, S(17), UI_RGBA(255, 255, 255, 0.08f * close.hover_t * vis));
    ui_icon_close(ui, xc, S(15), ui_alpha(ui_mix(UI_TEXT_DIM, UI_TEXT, close.hover_t), vis));

    /* pages: only the settled one takes input */
    b32 input = ui->input_enabled;
    if (pg < 0.99f) {
        ui->input_enabled = input && !app->browse_open;
        f32 a = vis * (1 - pg);
        draw_settings_main(app, vec2_make(p.x - S(40) * pg, p.y), w2, a);
    }
    if (pg > 0.01f) {
        ui->input_enabled = input && app->browse_open;
        draw_settings_browse(app, vec2_make(p.x + S(40) * (1 - pg), p.y), w2, h2, vis * pg);
    }
    ui->input_enabled = input;
    core_clip_pop(r);
    ui_block(ui, p, vec2_make(w2, h2));

    if (close.clicked) open_settings(app, false);
    if (live && in->key_pressed[PLATFORM_KEY_ESCAPE]) {
        if (app->browse_open) app->browse_open = false;
        else open_settings(app, false);
    }
    ui->input_enabled = true;
}

/* ------------------------------------------------------------------------- */
/* debug overlay                                                             */
/* ------------------------------------------------------------------------- */

static void draw_debug(App *app, f32 dt) {
    Ui *ui = &app->ui;
    if (dt > 0) app->fps_smooth = app->fps_smooth * 0.95f + (1.0f / dt) * 0.05f;
    Core_MemStats ms = core_mem_stats();
    Platform_MemInfo pm = platform_mem_info();
    Core_RenderStats rs = core_renderer_stats(app->r);
    Core_TextStats ts = core_text_stats(app->text);
    Covers_Stats cs = covers_stats(app->covers);
    f32 x = S(16), y = S(16);
    Core_String lines[6] = {
        str_fmt(app, "%.0f fps  %.2f ms", app->fps_smooth, dt * 1000.0f),
        str_fmt(app, "app memory %.1f MB  (arenas %.1f, heap %.1f, peak heap %.1f)",
                (ms.arena_committed + ms.heap_live) / 1048576.0, ms.arena_committed / 1048576.0,
                ms.heap_live / 1048576.0, ms.heap_peak / 1048576.0),
        str_fmt(app, "process RSS %.0f MB (anon %.0f, file %.0f) - includes GPU driver",
                pm.rss / 1048576.0, pm.rss_anon / 1048576.0, pm.rss_file / 1048576.0),
        str_fmt(app, "draw calls %u  instances %u  glyphs %u", rs.draw_calls, rs.instances, ts.glyphs),
        str_fmt(app, "covers %u/%u resident  %u pending  %u prefetch", cs.resident, cs.capacity, cs.pending, cs.prefetch_left),
        str_fmt(app, "library %u tracks  %u artists  queue %u", app->lib ? app->lib->track_count : 0,
                app->lib ? app->lib->artist_count : 0, app->queue_len),
    };
    Core_BoxStyle bg = { .radius = S(8), .fill = UI_RGBA(0, 0, 0, 0.6f) };
    core_draw_box(app->r, vec2_make(x - S(8), y - S(8)), vec2_make(S(470), S(20) * 6 + S(10)), &bg);
    for (u32 i = 0; i < 6; i++)
        ui_text(ui, ui->font_mono, lines[i], x, y + S(8) + S(20) * (f32)i, S(12), UI_RGBA(200, 255, 200, 1), UI_ALIGN_LEFT, 0);
}

/* ------------------------------------------------------------------------- */
/* demo script (headless screenshots / smoke tests)                          */
/* ------------------------------------------------------------------------- */

static void run_demo(App *app) {
    if (!app->demo || !app->lib) return;
    const char *s = app->demo;
    app->demo = 0;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", s);
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(0, ";")) {
        if (strncmp(tok, "search=", 7) == 0) {
            open_search(app, true);
            u32 n = (u32)strlen(tok + 7);
            memcpy(app->query, tok + 7, n);
            app->query_len = n;
            app->query_hash_done = 0;
            run_search(app);
        } else if (strncmp(tok, "tab=", 4) == 0) {
            app->tab = (u32)atoi(tok + 4) % TAB_COUNT;
        } else if (strncmp(tok, "play=", 5) == 0) {
            play_index(app, atoi(tok + 5), 0, false);
        } else if (strncmp(tok, "find=", 5) == 0) {
            Search_Hit h;
            if (search_tracks(app->lib, core_str(tok + 5), &h, 1)) {
                for (u32 i = 0; i < app->queue_len; i++)
                    if (app->queue[i] == h.track) { play_index(app, (s32)i, 0, false); break; }
            }
        } else if (strncmp(tok, "artist=", 7) == 0) {
            u32 ai = (u32)atoi(tok + 7);
            if (ai < app->lib->artist_count) { app->tab = TAB_ARTISTS; app->artist_open = (s32)ai; }
        } else if (strncmp(tok, "genre=", 6) == 0) {
            u32 gi = (u32)atoi(tok + 6);
            if (gi < app->lib->genre_count) { app->tab = TAB_GENRES; app->genre_open = (s32)gi; }
        } else if (strncmp(tok, "queueartist=", 12) == 0) {
            u32 ai = (u32)atoi(tok + 12);
            if (ai < app->lib->artist_count)
                queue_append_many(app, app->lib->artists[ai].tracks, app->lib->artists[ai].track_count);
        } else if (strncmp(tok, "seek=", 5) == 0) {
            player_seek(app->player, atof(tok + 5));
        } else if (strcmp(tok, "pause") == 0) {
            player_set_paused(app->player, true);
        } else if (strcmp(tok, "palmenu") == 0) {
            if (app->hit_count) {
                open_menu(app, app->hits[0].track, -1, true);
                app->menu_pos = vec2_make(400, 250);
            }
        } else if (strcmp(tok, "debug") == 0) {
            app->settings.debug = true;
        } else if (strcmp(tok, "settings") == 0) {
            open_settings(app, true);
        } else if (strncmp(tok, "browse", 6) == 0) {
            open_settings(app, true);
            open_browse(app, true);
            if (tok[6] == '=') browse_to(app, tok + 7);
        } else if (strncmp(tok, "folder=", 7) == 0) {
            set_music_dir(app, tok + 7);
        } else if (strncmp(tok, "theme=", 6) == 0) {
            for (u32 i = 0; i < SETTINGS_THEME_COUNT; i++)
                if (strcmp(SETTINGS_THEMES[i].id, tok + 6) == 0) { app->settings.theme = i; settings_theme_matrix(i, app->theme_m); }
        } else if (strncmp(tok, "vis=", 4) == 0) {
            for (u32 i = 0; i < VIS_COUNT; i++)
                if (strcmp(SETTINGS_VIS_IDS[i], tok + 4) == 0) app->settings.vis = i;
        } else if (strcmp(tok, "like") == 0) {
            const Lib_Track *t = current_track(app);
            if (t) set_liked(app, t->path_hash, true);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* frame                                                                     */
/* ------------------------------------------------------------------------- */

static void handle_shortcuts(App *app, const Platform_Input *in) {
    if (in->ctrl && in->key_pressed[PLATFORM_KEY_K]) { open_settings(app, false); open_search(app, !app->search_open); return; }
    if (in->ctrl && in->key_pressed[PLATFORM_KEY_COMMA]) { open_search(app, false); open_settings(app, !app->settings_open); return; }
    if (in->key_pressed[PLATFORM_KEY_F1]) { app->settings.debug = !app->settings.debug; save_settings(app); }
    if (in->key_pressed[PLATFORM_KEY_MEDIA_PLAY_PAUSE]) toggle_pause(app);
    if (in->key_pressed[PLATFORM_KEY_MEDIA_NEXT]) skip(app, +1);
    if (in->key_pressed[PLATFORM_KEY_MEDIA_PREV]) skip(app, -1);
    if (app->search_open || app->menu || app->settings_open) return;

    /* typing a letter opens search with it (fast path to find anything) */
    if (in->text_len && !in->ctrl && !in->alt && in->text[0] != ' ') {
        open_search(app, true);
        /* start from an empty query; draw_palette appends this frame's text */
        app->query_len = 0;
        app->query_hash_done = 0;
        return;
    }
    if (in->key_pressed[PLATFORM_KEY_SPACE]) toggle_pause(app);
    if (in->shift && in->key_pressed[PLATFORM_KEY_DELETE] && app->tab == TAB_QUEUE) queue_clear(app);
    Player_Status st = player_status(app->player);
    if (in->key_pressed[PLATFORM_KEY_RIGHT]) {
        if (in->ctrl || in->shift) skip(app, +1);
        else if (st.loaded) player_seek(app->player, st.position_s + 5);
    }
    if (in->key_pressed[PLATFORM_KEY_LEFT]) {
        if (in->ctrl || in->shift) skip(app, -1);
        else if (st.loaded) player_seek(app->player, CORE_MAX(0.0, st.position_s - 5));
    }
    if (in->key_pressed[PLATFORM_KEY_UP] || in->key_pressed[PLATFORM_KEY_DOWN]) {
        f32 d = in->key_pressed[PLATFORM_KEY_UP] ? 0.05f : -0.05f;
        app->volume = CORE_CLAMP(app->volume + d, 0.0f, 1.0f);
        player_set_volume(app->player, app->volume);
        app->volume_toast = 1.2f;
        app->state_dirty = true;
    }
    if (in->key_pressed[PLATFORM_KEY_TAB]) {
        app->tab = (app->tab + (in->shift ? TAB_COUNT - 1 : 1)) % TAB_COUNT;
    }
}

/* Borderless window: drag from empty chrome areas, resize from edges. */
static void handle_window_chrome(App *app, vec2 win, f32 left_w) {
    Ui *ui = &app->ui;
    const Platform_Input *in = ui->in;
    if (!in->mouse_inside) return;
    vec2 m = in->mouse_pos;
    b32 maximized = platform_window_is_maximized(app->win);
    f32 edge = 6;
    Platform_Edge e = PLATFORM_EDGE_NONE;
    if (!maximized) {
        b32 l = m.x < edge, rgt = m.x > win.x - edge, t = m.y < edge, b = m.y > win.y - edge;
        if (t && l) e = PLATFORM_EDGE_TOP_LEFT;
        else if (t && rgt) e = PLATFORM_EDGE_TOP_RIGHT;
        else if (b && l) e = PLATFORM_EDGE_BOTTOM_LEFT;
        else if (b && rgt) e = PLATFORM_EDGE_BOTTOM_RIGHT;
        else if (l) e = PLATFORM_EDGE_LEFT;
        else if (rgt) e = PLATFORM_EDGE_RIGHT;
        else if (t) e = PLATFORM_EDGE_TOP;
        else if (b) e = PLATFORM_EDGE_BOTTOM;
    }
    if (e != PLATFORM_EDGE_NONE && !ui->active) {
        static const Platform_Cursor cur[] = {
            [PLATFORM_EDGE_TOP] = PLATFORM_CURSOR_RESIZE_VERTICAL, [PLATFORM_EDGE_BOTTOM] = PLATFORM_CURSOR_RESIZE_VERTICAL,
            [PLATFORM_EDGE_LEFT] = PLATFORM_CURSOR_RESIZE_HORIZONTAL, [PLATFORM_EDGE_RIGHT] = PLATFORM_CURSOR_RESIZE_HORIZONTAL,
            [PLATFORM_EDGE_TOP_LEFT] = PLATFORM_CURSOR_RESIZE_NWSE, [PLATFORM_EDGE_BOTTOM_RIGHT] = PLATFORM_CURSOR_RESIZE_NWSE,
            [PLATFORM_EDGE_TOP_RIGHT] = PLATFORM_CURSOR_RESIZE_NESW, [PLATFORM_EDGE_BOTTOM_LEFT] = PLATFORM_CURSOR_RESIZE_NESW,
        };
        ui->cursor = cur[e];
        if (in->mouse_pressed[PLATFORM_MOUSE_LEFT]) { platform_window_begin_resize(app->win, e); return; }
    }
    if (!in->mouse_pressed[PLATFORM_MOUSE_LEFT] || ui->mouse_taken || app->search_open || app->settings_open) return;
    /* drag strip: top of the window, and empty stage corners */
    b32 top = m.y < S(44);
    b32 stage_empty = m.x < left_w && m.y < S(120);
    if (top || stage_empty) {
        if (ui->double_click) platform_window_toggle_maximize(app->win);
        else platform_window_begin_move(app->win);
    }
}

b32 app_frame(App *app, const Platform_Input *in, f32 dt, u32 width, u32 height) {
    Ui *ui = &app->ui;
    Core_Renderer *r = app->r;
    core_arena_reset(&app->frame);
    app->now += dt;

    vec2 win = vec2_make((f32)width, (f32)height);
    f32 scale = CORE_CLAMP(CORE_MIN(win.x / DESIGN_W, win.y / DESIGN_H), 0.62f, 1.6f);
    ui_begin(ui, in, dt, win, scale);
    core_text_begin_frame(app->text);

    poll_scanner(app);
    run_demo(app);
    covers_update(app->covers, dt);

    /* ---- engine state ---- */
    Player_Status st = player_status(app->player);
    if (st.advance_count != app->seen_advance) {
        app->seen_advance = st.advance_count;
        s32 n = next_index(app);
        if (n >= 0) {
            app->cur = n;
            const Lib_Track *t = current_track(app);
            if (t) { covers_set_art(app->covers, t); app->playing_hash = t->path_hash; }
            app->queue_follow = app->tab == TAB_QUEUE && app->scroll[TAB_QUEUE].idle > 3.0f;
            app->next_sent = false;
            app->state_dirty = true;
        }
    }
    sync_next(app);
    b32 playing = st.loaded && !st.paused && !st.ended;

    /* visualization input */
    u32 got = playing ? player_vis_samples(app->player, app->vis_samples, SPECTRUM_FFT) : 0;
    spectrum_update(&app->spec, app->vis_samples, got, st.sample_rate ? st.sample_rate : 48000, dt);
    /* the ribbon glides to a stop when paused so an idle player costs 0% CPU */
    f32 flow = ui_ease(ui, ui_id("vis.flow"), playing ? 1.0f : 0.0f, 1.5f);
    app->vis_time += dt * (1.25f * flow + 0.6f * app->spec.level);

    handle_shortcuts(app, in);
    if (app->volume_toast > 0) { app->volume_toast -= dt; ui->animating = true; }

    /* theme: glide the color matrix toward the chosen theme */
    f32 theme_target[9];
    b32 themed = settings_theme_matrix(app->settings.theme, theme_target);
    f32 tk = 1.0f - expf(-dt * 7.0f);
    for (u32 i = 0; i < 9; i++) {
        f32 d = theme_target[i] - app->theme_m[i];
        if (fabsf(d) > 0.0005f) { app->theme_m[i] += d * tk; ui->animating = true; themed = true; }
        else app->theme_m[i] = theme_target[i];
    }
    core_set_color_matrix(r, themed ? app->theme_m : 0);

    /* ---- layout ---- */
    f32 panel_w = roundf(CORE_CLAMP(win.x * 0.3256f, S(360), S(520)));
    f32 left_w = win.x - panel_w;
    f32 H = win.y;
    b32 modal = app->search_open || app->menu != MENU_NONE || app->settings_open;

    core_renderer_begin_frame(r, (vec4){0});
    Scene_Colors sc = scene_colors(app);
    draw_background(app, win, sc);

    ui->input_enabled = !modal;

    /* ---- stage ---- */
    f32 cx = left_w * 0.54f;
    vec2 C = vec2_make(left_w * 0.533f, H * 0.368f);
    f32 R = CORE_MIN(H * 0.315f, left_w * 0.36f);
    f32 stage_alpha = ui_ease(ui, ui_id("stage.alpha"), app->lib ? 1.0f : 0.5f, 3.0f);
    /* visualizer, crossfading when the setting changes */
    f32 vis_silk = ui_ease(ui, ui_id("vis.silk"), app->settings.vis == VIS_SILK ? 1.0f : 0.0f, 6.0f);
    f32 vis_bars = ui_ease(ui, ui_id("vis.bars"), app->settings.vis == VIS_BARS ? 1.0f : 0.0f, 6.0f);
    if (vis_silk > 0.004f) draw_ribbon(app, C, R, sc, vis_silk * stage_alpha * (0.75f + 0.25f * flow));
    if (vis_bars > 0.004f) draw_bars(app, C, R, sc, vis_bars * stage_alpha);
    f32 dur = (f32)st.duration_s;
    f32 progress = dur > 0 ? (f32)(st.position_s / dur) : 0;
    draw_ring(app, C, R, progress, st.duration_s);
    draw_cover_card(app, vec2_make(C.x + S(6), C.y + S(22)), R * 0.95f);

    f32 ty = H * 0.750f;
    draw_titles(app, cx, ty, H * 0.783f);
    draw_heart(app, vec2_make(cx, H * 0.810f));
    f32 seek_half = CORE_MIN(S(253), left_w * 0.3f);
    draw_seek_bar(app, cx - seek_half, cx + seek_half, H * 0.8265f, &st);
    f32 ty2 = H * 0.904f;
    draw_transport(app, cx, ty2, &st);
    draw_volume_toast(app, cx, H * 0.70f);

    /* volume by wheel over the stage (lists scroll themselves) */
    if (ui_mouse_in(ui, vec2_zero(), vec2_make(left_w, H)) && in->scroll_y != 0 && !modal) {
        app->volume = CORE_CLAMP(app->volume + in->scroll_y * (in->scroll_precise ? 0.01f : 0.04f), 0.0f, 1.0f);
        player_set_volume(app->player, app->volume);
        app->volume_toast = 1.2f;
        app->state_dirty = true;
    }

    /* ---- panel ---- */
    draw_panel(app, left_w, panel_w, H, playing);

    handle_window_chrome(app, win, left_w);

    ui_draw_effects(ui);
    draw_palette(app, win);
    draw_settings(app, win);
    draw_menu(app, win); /* above the palette, so it can be opened from a result row */
    if (app->settings.debug) draw_debug(app, dt);

    /* window outline + rounded corners (transparent window) */
    b32 maximized = platform_window_is_maximized(app->win);
    if (!maximized) {
        Core_BoxStyle outline = { .radius = 12, .fill = UI_RGBA(0, 0, 0, 0), .border = 1,
                                  .border_color = UI_RGBA(255, 255, 255, 0.10f) };
        core_draw_box(r, vec2_zero(), win, &outline);
    }
    core_renderer_end_frame(r);
    if (!maximized) core_renderer_mask_rounded(r, vec2_zero(), win, 12);

    ui_end(ui);
    platform_window_set_cursor(app->win, ui->cursor);

    /* persist occasionally (never on the hot path of an interaction) */
    if (app->state_dirty && app->now - app->last_state_save > 5.0) save_state(app);

    Covers_Stats cst = covers_stats(app->covers);
    b32 busy = ui->animating || playing || app->scanner != 0 || app->search_open ||
               cst.pending > 0 || app->spec.level > 0.001f;
    if (platform_env("OFFBEAT_DEBUG_BUSY") && busy)
        fprintf(stderr, "busy: anim=%d playing=%d scanner=%d search=%d pending=%u level=%f\n",
                ui->animating, playing, app->scanner != 0, app->search_open, cst.pending, app->spec.level);
    return busy;
}
