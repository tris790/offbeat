/*
 * Entry point. Deliberately thin: it wires the layers together and runs the
 * frame loop. All real work lives in platform_* (OS), core_* (reusable engine)
 * and the game/ app layer.
 *
 * Frame pacing: while anything moves (music playing, animations, background
 * work) we render every vsync; otherwise the loop blocks on input so an idle
 * player costs zero CPU.
 *
 * Headless screenshots (used for development/CI):
 *   OFFBEAT_SHOT=out.png [OFFBEAT_SHOT_FRAMES=90] [OFFBEAT_SIZE=1376x972]
 *   [OFFBEAT_DEMO="find=midnight;search=mid"] ./build/offbeat
 *
 * Headless recording (README gifs): every frame is written as raw RGBA to a
 * file or fifo, at real-time pace (audio-driven visuals stay in sync):
 *   OFFBEAT_REC=/tmp/rec.fifo OFFBEAT_SHOT_FRAMES=1800 ./build/offbeat
 */

#include "core/types.h"
#include "core/memory.h"
#include "core/renderer.h"
#include "platform/platform.h"
#include "game/app.h"

#include "third_party/stb_image_write.h"

#include <stdio.h>
#include <stdlib.h>

/* Window geometry lives in its own tiny file next to the app state: the window
   must exist before the app (and its state) is created. */
static void window_state_path(Core_Arena *arena, char *out, u64 cap)
{
    const char *dir = platform_env("OFFBEAT_CONFIG_DIR");
    if (!dir) dir = platform_config_dir(arena);
    snprintf(out, cap, "%s/window", dir);
}

static void window_state_load(Core_Arena *arena, const char *path, u32 *w, u32 *h, b32 *maximized)
{
    Core_Temp tmp = core_temp_begin(arena);
    Core_String s = platform_file_read_all(arena, path);
    unsigned sw, sh, sm = 0;
    if (s.len && sscanf((const char *)s.str, "size %u %u\nmaximized %u", &sw, &sh, &sm) >= 2 &&
        sw >= 720 && sh >= 480 && sw <= 16384 && sh <= 16384) {
        *w = sw;
        *h = sh;
        *maximized = sm != 0;
    }
    core_temp_end(tmp);
}

int main(void)
{
    const char *shot = platform_env("OFFBEAT_SHOT");
    const char *rec = platform_env("OFFBEAT_REC");
    b32 headless = shot || rec;
    u32 win_w = 1376, win_h = 972;
    b32 win_maximized = false;
    char state_path[1024] = {0};
    if (!headless) {
        Core_Arena scratch;
        if (core_arena_init(&scratch, 1 << 20)) {
            window_state_path(&scratch, state_path, sizeof(state_path));
            window_state_load(&scratch, state_path, &win_w, &win_h, &win_maximized);
            core_arena_release(&scratch);
        }
    }
    if (platform_env("OFFBEAT_SIZE")) sscanf(platform_env("OFFBEAT_SIZE"), "%ux%u", &win_w, &win_h);
    u32 shot_frames = platform_env("OFFBEAT_SHOT_FRAMES") ? (u32)atoi(platform_env("OFFBEAT_SHOT_FRAMES")) : 90;

    u32 flags = PLATFORM_WINDOW_TRANSPARENT;
    if (headless) {
        flags |= PLATFORM_WINDOW_HEADLESS;
        setenv("OFFBEAT_MUTE", "1", 0); /* screenshots never make sound */
    }

    Platform_Window *win = platform_window_open("Offbeat", "offbeat", win_w, win_h, flags);
    if (!win) {
        fprintf(stderr, "failed to open window\n");
        return 1;
    }

    if (win_maximized) platform_window_toggle_maximize(win);

    Core_Arena arena;
    if (!core_arena_init(&arena, CORE_GB(64))) {
        fprintf(stderr, "failed to reserve arena\n");
        return 1;
    }

    Core_Renderer *renderer = core_renderer_create(&arena, platform_gl_loader(win));
    if (!renderer) {
        fprintf(stderr, "failed to create renderer\n");
        return 1;
    }
    if (headless) core_renderer_use_offscreen(renderer, win_w, win_h);
    else {
        platform_window_size(win, &win_w, &win_h);
        core_renderer_resize(renderer, win_w, win_h);
    }

    App *app = app_create(&arena, renderer, win);

    FILE *rec_out = 0;
    u8 *rec_px = 0;
    if (rec) {
        rec_out = fopen(rec, "wb");
        if (!rec_out) {
            fprintf(stderr, "cannot open %s\n", rec);
            return 1;
        }
        rec_px = core_heap_alloc((u64)win_w * win_h * 4);
    }

    Platform_Input input = {0};
    f64 last = platform_time_seconds();
    u32 frame = 0;
    b32 busy = true;
    u32 restore_w = win_w, restore_h = win_h; /* last un-maximized size */

    while (!platform_window_should_close(win)) {
        if (!busy && !headless) platform_wait_events(win, 0.5);
        platform_poll_events(win, &input);

        u32 w, h;
        platform_window_size(win, &w, &h);
        if (!headless && (w != win_w || h != win_h)) {
            win_w = w;
            win_h = h;
            core_renderer_resize(renderer, win_w, win_h);
        }
        if (!platform_window_is_maximized(win)) {
            restore_w = w;
            restore_h = h;
        }

        f64 now = platform_time_seconds();
        f32 dt = (f32)(now - last);
        last = now;
        if (headless) dt = 1.0f / 60.0f;    /* deterministic animation */
        if (dt > 0.1f) dt = 0.1f;           /* after a sleep: no huge jumps */

        busy = app_frame(app, &input, dt, win_w, win_h);
        platform_swap_buffers(win);
        frame++;

        if (rec) {
            core_renderer_read_pixels(renderer, win_w, win_h, rec_px);
            if (fwrite(rec_px, 4, (u64)win_w * win_h, rec_out) != (u64)win_w * win_h) break;
            /* real-time pace: the audio clock (positions, spectrum) is wall-clock */
            f64 spare = (1.0 / 60.0) - (platform_time_seconds() - now);
            if (spare > 0) platform_sleep(spare);
            if (frame >= shot_frames) break;
        } else if (shot) {
            /* let background loading (covers, audio) catch up between frames */
            platform_sleep(0.004);
            if (frame >= shot_frames) {
                u8 *px = core_heap_alloc((u64)win_w * win_h * 4);
                core_renderer_read_pixels(renderer, win_w, win_h, px);
                stbi_write_png(shot, (int)win_w, (int)win_h, 4, px, (int)win_w * 4);
                core_heap_free(px);
                printf("wrote %s\n", shot);
                break;
            }
        }
    }

    if (rec_out) fclose(rec_out);
    app_shutdown(app); /* also creates the config dir */

    if (!headless && state_path[0]) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "size %u %u\nmaximized %u\n", restore_w, restore_h,
                         platform_window_is_maximized(win) ? 1u : 0u);
        platform_file_write_all(state_path, buf, (u64)n);
    }

    platform_window_close(win);
    return 0;
}
