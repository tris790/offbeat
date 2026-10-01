#ifndef PLATFORM_H
#define PLATFORM_H

#include "../core/math.h"
#include "../core/memory.h"
#include "../core/string.h"
#include "../core/renderer.h"

/*
 * Platform abstraction: window, GL context, input, timing, threads, files and
 * audio output.
 *
 * All OS-specific code lives behind this header. The Linux backends are
 * platform_linux_wayland.c (window/input), platform_posix.c (threads, files,
 * time) and platform_linux_alsa.c (audio). A Windows port implements the same
 * functions; game and core code include only this header.
 */

typedef struct Platform_Window Platform_Window;

/* Keyboard keys we care about (extend as needed). Values are arbitrary. */
typedef enum {
    PLATFORM_KEY_UNKNOWN = 0,
    PLATFORM_KEY_ESCAPE,
    PLATFORM_KEY_ENTER,
    PLATFORM_KEY_SPACE,
    PLATFORM_KEY_TAB,
    PLATFORM_KEY_BACKSPACE,
    PLATFORM_KEY_DELETE,
    PLATFORM_KEY_HOME,
    PLATFORM_KEY_END,
    PLATFORM_KEY_PAGE_UP,
    PLATFORM_KEY_PAGE_DOWN,
    PLATFORM_KEY_LEFT,
    PLATFORM_KEY_RIGHT,
    PLATFORM_KEY_UP,
    PLATFORM_KEY_DOWN,
    PLATFORM_KEY_A, PLATFORM_KEY_B, PLATFORM_KEY_C, PLATFORM_KEY_D,
    PLATFORM_KEY_E, PLATFORM_KEY_F, PLATFORM_KEY_G, PLATFORM_KEY_H,
    PLATFORM_KEY_I, PLATFORM_KEY_J, PLATFORM_KEY_K, PLATFORM_KEY_L,
    PLATFORM_KEY_M, PLATFORM_KEY_N, PLATFORM_KEY_O, PLATFORM_KEY_P,
    PLATFORM_KEY_Q, PLATFORM_KEY_R, PLATFORM_KEY_S, PLATFORM_KEY_T,
    PLATFORM_KEY_U, PLATFORM_KEY_V, PLATFORM_KEY_W, PLATFORM_KEY_X,
    PLATFORM_KEY_Y, PLATFORM_KEY_Z,
    PLATFORM_KEY_F1, PLATFORM_KEY_F2,
    PLATFORM_KEY_COMMA,
    PLATFORM_KEY_MEDIA_PLAY_PAUSE,
    PLATFORM_KEY_MEDIA_NEXT,
    PLATFORM_KEY_MEDIA_PREV,
    PLATFORM_KEY_COUNT
} Platform_Key;

typedef enum {
    PLATFORM_MOUSE_LEFT = 0,
    PLATFORM_MOUSE_RIGHT,
    PLATFORM_MOUSE_MIDDLE,
    PLATFORM_MOUSE_BUTTON_COUNT
} Platform_MouseButton;

/* Theme-backed cursors shared by all platform backends. */
typedef enum {
    PLATFORM_CURSOR_DEFAULT = 0,
    PLATFORM_CURSOR_TEXT,
    PLATFORM_CURSOR_HAND,
    PLATFORM_CURSOR_MOVE,
    PLATFORM_CURSOR_GRAB,
    PLATFORM_CURSOR_GRABBING,
    PLATFORM_CURSOR_RESIZE_HORIZONTAL,
    PLATFORM_CURSOR_RESIZE_VERTICAL,
    PLATFORM_CURSOR_RESIZE_NWSE,
    PLATFORM_CURSOR_RESIZE_NESW,
    PLATFORM_CURSOR_CROSSHAIR,
    PLATFORM_CURSOR_NOT_ALLOWED,
    PLATFORM_CURSOR_HIDDEN,
    PLATFORM_CURSOR_COUNT
} Platform_Cursor;

/* Per-frame input snapshot. Edge flags (`*_pressed`/`*_released`) are true only
   on the frame the transition happened; held state is in `*_down`. Held keys
   auto-repeat: `key_pressed` fires again at the compositor's repeat rate, and
   repeated characters are appended to `text`. */
typedef struct {
    /* keyboard */
    b32 key_down[PLATFORM_KEY_COUNT];
    b32 key_pressed[PLATFORM_KEY_COUNT];
    b32 key_released[PLATFORM_KEY_COUNT];
    b32 ctrl, shift, alt; /* modifier state */

    /* text input as UTF-8 (this frame's typed characters) */
    u8  text[64];
    u32 text_len;

    /* mouse */
    vec2 mouse_pos;       /* in pixels, top-left origin */
    vec2 mouse_delta;     /* movement since last frame  */
    b32 mouse_down[PLATFORM_MOUSE_BUTTON_COUNT];
    b32 mouse_pressed[PLATFORM_MOUSE_BUTTON_COUNT];
    b32 mouse_released[PLATFORM_MOUSE_BUTTON_COUNT];
    b32 mouse_inside;       /* pointer is over the window */
    f32 scroll_x, scroll_y; /* wheel delta this frame, in "lines" (+y = up) */
    b32 scroll_precise;     /* delta came from a touchpad (smooth)          */

    b32 focused;            /* window has keyboard focus */
    b32 any_event;          /* something happened since last poll */
} Platform_Input;

/* ---- window lifecycle ---- */

/* Window feature flags, OR'd together and passed to platform_window_open.
   Platform-neutral: each backend maps a flag to its own facility and silently
   ignores any it cannot honor. */
typedef enum {
    PLATFORM_WINDOW_DECORATED   = 1 << 0, /* OS/compositor draws titlebar + min/max/close */
    PLATFORM_WINDOW_TRANSPARENT = 1 << 1, /* framebuffer alpha is composited (rounded corners) */
    PLATFORM_WINDOW_HEADLESS    = 1 << 2, /* no visible window; GL context only (screenshots/tests) */
} Platform_WindowFlags;

Platform_Window *platform_window_open(const char *title, const char *app_id,
                                      u32 width, u32 height, u32 flags);
void             platform_window_close(Platform_Window *win);

/* True until the user/compositor asks the window to close. */
b32  platform_window_should_close(Platform_Window *win);
void platform_window_request_close(Platform_Window *win);

/* Pump events; fills the input snapshot for this frame. */
void platform_poll_events(Platform_Window *win, Platform_Input *out_input);

/* Block until an input event arrives or `timeout_s` elapses. Used to idle at
   zero CPU when nothing is animating. */
void platform_wait_events(Platform_Window *win, f64 timeout_s);

/* Select the OS cursor. Repeating the current value is a cheap no-op. */
void platform_window_set_cursor(Platform_Window *win, Platform_Cursor cursor);

/* Swap buffers / present the rendered frame. */
void platform_swap_buffers(Platform_Window *win);

/* Current drawable size in pixels. */
void platform_window_size(Platform_Window *win, u32 *out_w, u32 *out_h);

/* Client-side window chrome (for undecorated windows that draw their own
   titlebar). Must be called while handling a mouse press. */
typedef enum {
    PLATFORM_EDGE_NONE = 0,
    PLATFORM_EDGE_TOP, PLATFORM_EDGE_BOTTOM, PLATFORM_EDGE_LEFT, PLATFORM_EDGE_RIGHT,
    PLATFORM_EDGE_TOP_LEFT, PLATFORM_EDGE_TOP_RIGHT,
    PLATFORM_EDGE_BOTTOM_LEFT, PLATFORM_EDGE_BOTTOM_RIGHT,
} Platform_Edge;

void platform_window_begin_move(Platform_Window *win);
void platform_window_begin_resize(Platform_Window *win, Platform_Edge edge);
void platform_window_minimize(Platform_Window *win);
void platform_window_toggle_maximize(Platform_Window *win);
b32  platform_window_is_maximized(Platform_Window *win);

/* GL proc loader to hand to core_renderer_create. */
Core_GlLoadProc platform_gl_loader(Platform_Window *win);

/* ---- timing ---- */

f64  platform_time_seconds(void); /* monotonic seconds since some epoch */
void platform_sleep(f64 seconds);

/* ---- threads & sync ---- */

typedef struct Platform_Thread Platform_Thread;
typedef void (*Platform_ThreadProc)(void *user);

/* Opaque storage big enough for the native primitives (pthread on Linux). */
typedef struct { _Alignas(16) u8 opaque[64]; } Platform_Mutex;
typedef struct { _Alignas(16) u8 opaque[64]; } Platform_Cond;

Platform_Thread *platform_thread_start(Platform_ThreadProc proc, void *user, const char *name);
void             platform_thread_join(Platform_Thread *t);
/* Lower the calling thread's scheduling priority (background workers). */
void             platform_thread_set_background(void);
u32              platform_cpu_count(void);

void platform_mutex_init(Platform_Mutex *m);
void platform_mutex_destroy(Platform_Mutex *m);
void platform_mutex_lock(Platform_Mutex *m);
void platform_mutex_unlock(Platform_Mutex *m);

void platform_cond_init(Platform_Cond *c);
void platform_cond_destroy(Platform_Cond *c);
void platform_cond_wait(Platform_Cond *c, Platform_Mutex *m);
/* Returns false on timeout. */
b32  platform_cond_wait_timeout(Platform_Cond *c, Platform_Mutex *m, f64 seconds);
void platform_cond_signal(Platform_Cond *c);
void platform_cond_broadcast(Platform_Cond *c);

/* ---- files ---- */

typedef struct { s64 handle; } Platform_File; /* handle < 0 == invalid */

Platform_File platform_file_open_read(const char *path);
b32           platform_file_valid(Platform_File f);
u64           platform_file_size(Platform_File f);
/* Positional read; returns bytes read (may be short at EOF). Thread-safe. */
u64           platform_file_read_at(Platform_File f, u64 offset, void *dst, u64 size);
void          platform_file_close(Platform_File f);

/* Read a whole file into `arena`. Returns {0} on failure. */
Core_String   platform_file_read_all(Core_Arena *arena, const char *path);
/* Atomically replace `path` with `data` (write temp + rename). */
b32           platform_file_write_all(const char *path, const void *data, u64 size);

typedef struct {
    b32 exists;
    b32 is_dir;
    u64 size;
    s64 mtime_ns;
} Platform_FileInfo;

Platform_FileInfo platform_file_info(const char *path);
/* mkdir -p */
b32               platform_make_dirs(const char *path);
/* Rename within one filesystem, replacing `to` if it exists. */
b32               platform_file_rename(const char *from, const char *to);
/* Delete a file. */
b32               platform_file_remove(const char *path);
/* Delete a directory and everything under it (never follows symlinks). */
void              platform_remove_tree(const char *path);
/* rmdir: removes `path` only if it is an empty directory (true when it did). */
b32               platform_remove_dir_if_empty(const char *path);

/* Recursive directory walk. Calls `visit` for every regular file (symlinks
   followed, hidden entries skipped). `visit` returns false to stop the walk. */
typedef b32 (*Platform_WalkProc)(void *user, const char *path, u64 size, s64 mtime_ns);
void platform_walk_dir(const char *root, Platform_WalkProc visit, void *user);

/* Immediate subdirectories of `path` (symlinks followed, hidden entries
   skipped), sorted case-insensitively. Names live in `arena`. Returns the
   count; 0 when `path` can't be read. */
u32  platform_list_dirs(Core_Arena *arena, const char *path, const char ***out_names);

/* Well-known directories (no trailing slash). Strings live in `arena`. */
const char *platform_home_dir(Core_Arena *arena);
const char *platform_cache_dir(Core_Arena *arena);  /* e.g. ~/.cache/<app>   */
const char *platform_config_dir(Core_Arena *arena); /* e.g. ~/.config/<app>  */
const char *platform_exe_dir(Core_Arena *arena);    /* dir containing binary */
const char *platform_music_dir(Core_Arena *arena);  /* e.g. ~/Music          */

/* Map a whole file read-only (shared, file-backed pages). 0 on failure. */
const u8   *platform_file_map(const char *path, u64 *out_size);

/* Environment variable or 0. */
const char *platform_env(const char *name);
/* Set an environment variable; overwrite=false preserves an existing value. */
b32 platform_env_set(const char *name, const char *value, b32 overwrite);

/* ---- child processes ----
 *
 * Just enough to drive command-line tools (yt-dlp, ffmpeg): start one with its
 * stdout+stderr merged into a pipe, read it line by line, stop it. The child
 * runs in its own process group so stopping it also stops whatever it spawned.
 */

typedef struct Platform_Process Platform_Process;

/* Start `argv[0]` (looked up in PATH), argv NULL-terminated, stdin closed.
   Returns 0 if it could not be started. */
Platform_Process *platform_process_spawn(const char *const *argv);
/* Next line of output without the line ending (truncated to `cap - 1` bytes),
   waiting up to `timeout_s`. Returns its length (>= 0), -1 once the child has
   closed its output, or -2 when the wait timed out. */
s32               platform_process_read_line(Platform_Process *p, char *buf, u32 cap, f64 timeout_s);
/* Ask the child (and its process group) to stop. Safe from any thread while
   another thread is reading; the reader then sees end of output. */
void              platform_process_kill(Platform_Process *p);
/* Wait for exit and release the handle. Returns the exit status, or 128 + the
   signal number if it was killed. */
s32               platform_process_finish(Platform_Process *p);
/* Resolve an executable: a path is checked as is, a bare name is searched in
   PATH. Writes the full path to `out` and returns true when it is runnable. */
b32               platform_find_executable(const char *name, char *out, u64 cap);

/* Process memory as the OS sees it (resident set, and its anonymous part). */
typedef struct { u64 rss; u64 rss_anon; u64 rss_file; } Platform_MemInfo;
Platform_MemInfo platform_mem_info(void);

/* ---- audio output ----
 *
 * Blocking, push-style PCM sink: the audio thread decodes and pushes
 * interleaved f32 frames. The device runs at the stream's native rate; if the
 * rate or channel count changes, close and reopen.
 */

typedef struct Platform_Audio Platform_Audio;

/* `latency_s` is the requested buffer depth (e.g. 0.05). Returns 0 on failure.
   If OFFBEAT_MUTE is set, a silent sink that consumes in real time is used. */
Platform_Audio *platform_audio_open(u32 sample_rate, u32 channels, f64 latency_s);
void            platform_audio_close(Platform_Audio *a);
/* Blocks until all frames are queued. Returns frames written. */
u32             platform_audio_write(Platform_Audio *a, const f32 *frames, u32 frame_count);
/* Frames queued in the device that have not been heard yet. */
u32             platform_audio_delay(Platform_Audio *a);
/* Discard queued audio immediately (seek / skip) and get ready to write again. */
void            platform_audio_drop(Platform_Audio *a);
/* Block until queued audio has played out (pause after a fade). */
void            platform_audio_drain(Platform_Audio *a);
u32             platform_audio_rate(Platform_Audio *a);
u32             platform_audio_channels(Platform_Audio *a);

#endif /* PLATFORM_H */
