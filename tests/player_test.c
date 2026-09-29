/*
 * Audio engine test: decoders + mp3 seek accuracy, the player state machine
 * (open, position clock, seek, pause, auto-advance, end), memory, and the
 * spectrum analyzer. Uses real files from ~/Music through the muted sink
 * (OFFBEAT_MUTE), so it never makes a sound. Prints the measured latencies.
 */

#include "core/memory.c"
#include "core/string.c"
#include "platform/platform_posix.c"
#include "platform/platform_linux_alsa.c"
#include "game/player.c"
#include "game/spectrum.c"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int test_failures;

#define TEST_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "player test failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
        test_failures++; \
    } \
} while (0)

static f64 test_cpu_seconds(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

/* ---- file discovery ---- */

#define TEST_MAX_FILES 48
static char test_files[TEST_MAX_FILES][PLAYER_PATH_MAX];
static u32  test_file_count;

static b32 test_collect(void *user, const char *path, u64 size, s64 mtime_ns) {
    (void)user; (void)mtime_ns;
    size_t n = strlen(path);
    /* ordinary songs: 1..20 MB */
    if (n > 4 && n < PLAYER_PATH_MAX && !strcasecmp(path + n - 4, ".mp3") &&
        size > CORE_MB(1) && size < CORE_MB(20)) {
        memcpy(test_files[test_file_count++], path, n + 1);
    }
    return test_file_count < TEST_MAX_FILES;
}

/* ---- decoder level ---- */

/* Seek with the table vs. decoding linearly to the same frame; the audio must
   match (after the first mp3 frame, whose overlap state differs). */
static void test_seek_accuracy(const char *path, f64 fraction) {
    static f32 a[4096 * 2], b[4096 * 2], junk[PLAYER_CHUNK_FRAMES * PLAYER_MAX_CHANNELS];
    Player_Decoder da, db;
    if (!player_dec_open(&da, path) || !player_dec_open(&db, path)) {
        TEST_CHECK(!"open failed");
        return;
    }
    u64 target = (u64)((f64)da.total * fraction);
    f64 t0 = platform_time_seconds();
    player_dec_seek(&da, target);
    f64 t1 = platform_time_seconds();
    TEST_CHECK(da.cursor == target);
    u32 got_a = player_dec_read(&da, a, 4096);

    u64 left = target;
    while (left > 0) {
        u32 n = left > PLAYER_CHUNK_FRAMES ? PLAYER_CHUNK_FRAMES : (u32)left;
        u32 got = player_dec_read(&db, junk, n);
        if (!got) break;
        left -= got;
    }
    u32 got_b = player_dec_read(&db, b, 4096);
    TEST_CHECK(got_a == 4096 && got_b == 4096);

    f32 max_err = 0;
    for (u32 i = 1500 * da.channels; i < 4096 * da.channels; i++) {
        f32 e = fabsf(a[i] - b[i]);
        if (e > max_err) max_err = e;
    }
    printf("  seek %5.1f%% of %-50.50s  table seek %.2f ms, max diff %.5f\n",
           fraction * 100, strrchr(path, '/') + 1, (t1 - t0) * 1e3, max_err);
    TEST_CHECK(max_err < 1e-3f);
    player_dec_close(&da);
    player_dec_close(&db);
}

static void test_decoder(void) {
    printf("decoder:\n");
    /* open + full seek-table walk + decode speed on a handful of files */
    f64 open_sum = 0, open_max = 0, scan_sum = 0, scan_max = 0, cpu = 0, audio = 0;
    u32 n = test_file_count < 12 ? test_file_count : 12;
    for (u32 i = 0; i < n; i++) {
        Player_Decoder d;
        f64 t0 = platform_time_seconds();
        b32 ok = player_dec_open(&d, test_files[i]);
        f64 t1 = platform_time_seconds();
        TEST_CHECK(ok);
        if (!ok) continue;
        TEST_CHECK(d.total > 0 && d.rate > 0 && d.channels >= 1);
        while (d.scan) player_scan_step(&d, CORE_GB(1));
        f64 t2 = platform_time_seconds();
        TEST_CHECK(d.seek_count > 10);
        open_sum += t1 - t0; if (t1 - t0 > open_max) open_max = t1 - t0;
        scan_sum += t2 - t1; if (t2 - t1 > scan_max) scan_max = t2 - t1;

        if (i < 4) {
            static f32 buf[PLAYER_CHUNK_FRAMES * PLAYER_MAX_CHANNELS];
            f64 c0 = test_cpu_seconds(CLOCK_THREAD_CPUTIME_ID);
            u64 frames = 0;
            u32 got;
            while ((got = player_dec_read(&d, buf, PLAYER_CHUNK_FRAMES)) > 0) frames += got;
            cpu   += test_cpu_seconds(CLOCK_THREAD_CPUTIME_ID) - c0;
            audio += (f64)frames / (f64)d.rate;
            /* exact length: decoding ends where the header said */
            /* (encoder padding can make the header ~20-30 ms longer) */
            TEST_CHECK(frames + d.rate / 20 >= d.total && frames <= d.total + d.rate / 100);
        }
        player_dec_close(&d);
    }
    printf("  open (init, no scan): avg %.2f ms, max %.2f ms over %u files\n", open_sum / n * 1e3, open_max * 1e3, n);
    printf("  seek-table walk (whole file, incremental in playback): avg %.2f ms, max %.2f ms\n", scan_sum / n * 1e3, scan_max * 1e3);
    if (audio > 0) printf("  decode: %.2f ms CPU per second of audio (%.0fx realtime)\n", cpu / audio * 1e3, audio / cpu);

    u32 m = test_file_count < 5 ? test_file_count : 5;
    for (u32 i = 0; i < m; i++) test_seek_accuracy(test_files[i], 0.1 + 0.2 * (f64)i);
    if (test_file_count > 0) test_seek_accuracy(test_files[0], 0.0005); /* before the first seek point */
}

/* ---- player ---- */

static Player_Status test_wait_status(Player *p, f64 timeout, b32 (*done)(Player_Status *, u64), u64 arg) {
    f64 end = platform_time_seconds() + timeout;
    Player_Status s = player_status(p);
    while (!done(&s, arg) && platform_time_seconds() < end) {
        platform_sleep(0.001);
        s = player_status(p);
    }
    return s;
}
static b32 test_is_loaded(Player_Status *s, u64 load_count) { return s->load_count > load_count || s->error; }
static b32 test_is_advanced(Player_Status *s, u64 count)    { return s->advance_count > count; }
static b32 test_is_ended(Player_Status *s, u64 unused)      { (void)unused; return s->ended; }

static f64 test_cmd_latency(Player *p, f64 timeout) {
    f64 end = platform_time_seconds() + timeout;
    for (;;) {
        platform_mutex_lock(&p->mutex);
        f64 l = p->dbg_cmd_latency;
        platform_mutex_unlock(&p->mutex);
        if (l >= 0 || platform_time_seconds() > end) return l;
        platform_sleep(0.0005);
    }
}

static void test_reset_latency(Player *p) {
    platform_mutex_lock(&p->mutex);
    p->dbg_cmd_latency = -1;
    platform_mutex_unlock(&p->mutex);
}

static void test_player(void) {
    printf("player:\n");
    if (test_file_count < 2) { TEST_CHECK(!"need two mp3 files in ~/Music"); return; }
    const char *a = test_files[0], *b = test_files[1];
    u64 heap_before = core_mem_stats().heap_live;

    Player *p = player_create();
    TEST_CHECK(p != 0);
    player_set_volume(p, 0.8f);

    /* open -> first audio */
    f64 lat_sum = 0, lat_max = 0;
    for (u32 i = 0; i < 6 && i < test_file_count; i++) {
        test_reset_latency(p);
        u64 lc = player_status(p).load_count;
        player_play_file(p, test_files[i], 1000 + i, 0, false);
        Player_Status s = test_wait_status(p, 2.0, test_is_loaded, lc);
        TEST_CHECK(s.loaded && !s.error && s.track_token == 1000 + i);
        TEST_CHECK(s.duration_s > 1 && s.sample_rate > 0 && s.channels > 0);
        f64 l = test_cmd_latency(p, 1.0);
        TEST_CHECK(l >= 0 && l < 0.05);
        lat_sum += l; if (l > lat_max) lat_max = l;
    }
    printf("  play -> first audio: avg %.2f ms, max %.2f ms\n", lat_sum / 6 * 1e3, lat_max * 1e3);

    /* position runs in real time and never steps back */
    u64 lc = player_status(p).load_count;
    player_play_file(p, a, 1, 0, false);
    Player_Status s = test_wait_status(p, 2.0, test_is_loaded, lc);
    TEST_CHECK(s.loaded && s.track_token == 1 && !s.paused);
    f64 duration = s.duration_s;
    platform_sleep(0.2);
    f64 p0 = player_status(p).position_s, t0 = platform_time_seconds();
    f64 last = p0, max_step = 0;
    b32 monotonic = true;
    while (platform_time_seconds() - t0 < 1.0) {
        platform_sleep(0.004);
        f64 pos = player_status(p).position_s;
        if (pos < last) monotonic = false;
        if (pos - last > max_step) max_step = pos - last;
        last = pos;
    }
    f64 speed = (last - p0) / (platform_time_seconds() - t0);
    printf("  position speed %.3fx realtime, largest step between polls %.1f ms\n", speed, max_step * 1e3);
    TEST_CHECK(speed > 0.9 && speed < 1.1);
    TEST_CHECK(monotonic);

    /* visualization history follows playback */
    {
        f32 vis[SPECTRUM_FFT];
        u32 got = player_vis_samples(p, vis, SPECTRUM_FFT);
        f32 energy = 0;
        for (u32 i = 0; i < got; i++) energy += vis[i] * vis[i];
        TEST_CHECK(got == SPECTRUM_FFT);
        TEST_CHECK(energy > 0);
    }

    /* CPU while playing (process-wide: audio thread + muted sink) */
    {
        f64 c0 = test_cpu_seconds(CLOCK_PROCESS_CPUTIME_ID);
        platform_sleep(1.0);
        f64 c1 = test_cpu_seconds(CLOCK_PROCESS_CPUTIME_ID);
        printf("  CPU while playing: %.1f ms per second\n", (c1 - c0) * 1e3);
    }
    printf("  heap while playing: %.1f KB live (peak %.1f KB)\n",
           (f64)(core_mem_stats().heap_live - heap_before) / 1024.0, (f64)core_mem_stats().heap_peak / 1024.0);
    TEST_CHECK(core_mem_stats().heap_live - heap_before < CORE_KB(512));

    /* seek near the end: instant, and the position jumps there */
    {
        f64 target = duration - 8.0;
        test_reset_latency(p);
        player_seek(p, target);
        f64 immediate = player_status(p).position_s;
        f64 l = test_cmd_latency(p, 1.0);
        platform_sleep(0.25);
        f64 after = player_status(p).position_s;
        printf("  seek to %.1fs: first audio after %.2f ms; position %.2f -> %.2f after 250 ms\n",
               target, l * 1e3, immediate, after);
        TEST_CHECK(l >= 0 && l < 0.05);
        TEST_CHECK(fabs(immediate - target) < 0.01);
        TEST_CHECK(after > target + 0.1 && after < target + 0.4);

        /* back to the start and forward again: both instant */
        f64 worst = 0;
        f64 targets[4] = { 1.0, duration * 0.5, 0.0, duration * 0.9 };
        for (u32 i = 0; i < 4; i++) {
            test_reset_latency(p);
            player_seek(p, targets[i]);
            f64 li = test_cmd_latency(p, 1.0);
            if (li > worst) worst = li;
            platform_sleep(0.02);
        }
        printf("  seek storm (start/middle/end): worst %.2f ms\n", worst * 1e3);
        TEST_CHECK(worst >= 0 && worst < 0.05);
    }

    /* pause stops the clock (after the ~25 ms fade + device drain) */
    {
        player_set_paused(p, true);
        TEST_CHECK(player_status(p).paused);
        platform_sleep(0.2);
        f64 a0 = player_status(p).position_s;
        f64 c0 = test_cpu_seconds(CLOCK_PROCESS_CPUTIME_ID);
        platform_sleep(0.4);
        f64 c1 = test_cpu_seconds(CLOCK_PROCESS_CPUTIME_ID);
        f64 a1 = player_status(p).position_s;
        f32 vis[64];
        printf("  paused: position %.3f -> %.3f, CPU %.2f ms over 400 ms\n", a0, a1, (c1 - c0) * 1e3);
        TEST_CHECK(fabs(a1 - a0) < 0.001);
        TEST_CHECK(c1 - c0 < 0.004);
        TEST_CHECK(player_vis_samples(p, vis, 64) == 0);

        test_reset_latency(p);
        player_set_paused(p, false);
        f64 l = test_cmd_latency(p, 1.0);
        platform_sleep(0.3);
        f64 a2 = player_status(p).position_s;
        printf("  resume: first audio after %.2f ms, position %.3f after 300 ms\n", l * 1e3, a2);
        TEST_CHECK(l >= 0 && l < 0.05);
        TEST_CHECK(a2 > a1 + 0.15);
    }

    /* auto-advance into `next` */
    {
        Player_Status before = player_status(p);
        player_set_next(p, b, 2);
        player_seek(p, duration - 1.0);
        s = test_wait_status(p, 4.0, test_is_advanced, before.advance_count);
        TEST_CHECK(s.advance_count == before.advance_count + 1);
        TEST_CHECK(s.load_count == before.load_count + 1);
        TEST_CHECK(s.track_token == 2 && !s.ended && !s.paused && s.loaded);
        platform_sleep(0.3);
        s = player_status(p);
        printf("  advanced to token %llu, position %.2f s after 300 ms\n", (unsigned long long)s.track_token, s.position_s);
        TEST_CHECK(s.position_s > 0.1 && s.position_s < 0.6);
    }

    /* end without `next`: stop at the end, paused; play restarts */
    {
        f64 d2 = player_status(p).duration_s;
        player_seek(p, d2 - 0.5);
        s = test_wait_status(p, 3.0, test_is_ended, 0);
        TEST_CHECK(s.ended && s.paused && s.track_token == 2);
        TEST_CHECK(fabs(s.position_s - d2) < 0.05);
        player_set_paused(p, false);
        platform_sleep(0.3);
        s = player_status(p);
        TEST_CHECK(!s.ended && !s.paused && s.position_s > 0.1 && s.position_s < 0.6);
    }

    /* start paused at an offset (session restore) */
    {
        lc = player_status(p).load_count;
        player_play_file(p, a, 3, 30.0, true);
        s = test_wait_status(p, 2.0, test_is_loaded, lc);
        platform_sleep(0.1);
        s = player_status(p);
        TEST_CHECK(s.loaded && s.paused && s.track_token == 3);
        TEST_CHECK(fabs(s.position_s - 30.0) < 0.05);
    }

    /* broken file */
    {
        lc = player_status(p).load_count;
        player_play_file(p, "/nonexistent/file.mp3", 4, 0, false);
        s = test_wait_status(p, 2.0, test_is_loaded, lc);
        TEST_CHECK(s.error && !s.loaded && s.paused);
    }

    player_destroy(p);
    printf("  heap after destroy: %lld bytes\n", (long long)(core_mem_stats().heap_live - heap_before));
    TEST_CHECK(core_mem_stats().heap_live == heap_before);
}

/* ---- spectrum ---- */

static u32 test_peak_band(f32 hz) {
    static Spectrum s;
    f32 x[SPECTRUM_FFT];
    memset(&s, 0, sizeof(s));
    for (u32 i = 0; i < SPECTRUM_FFT; i++) x[i] = 0.5f * sinf(2.0f * CORE_PI * hz * (f32)i / 48000.0f);
    for (u32 i = 0; i < 30; i++) spectrum_update(&s, x, SPECTRUM_FFT, 48000, 1.0f / 60.0f);
    u32 best = 0;
    for (u32 b = 1; b < SPECTRUM_BANDS; b++) if (s.bands[b] > s.bands[best]) best = b;
    return best;
}

static void test_spectrum(void) {
    printf("spectrum:\n");
    f32 freqs[] = { 35, 60, 100, 440, 1000, 3000, 8000, 14000 };
    spectrum_layout(48000);
    for (u32 i = 0; i < CORE_ARRAY_COUNT(freqs); i++) {
        u32 band = test_peak_band(freqs[i]);
        f32 lo = spectrum_band_lo[band] * 48000.0f / SPECTRUM_FFT;
        f32 hi = spectrum_band_hi[band] * 48000.0f / SPECTRUM_FFT;
        printf("  %6.0f Hz sine -> band %2u (%.0f..%.0f Hz)\n", freqs[i], band, lo, hi);
        /* the right band, or its neighbour when the tone sits on an edge;
           below ~60 Hz one FFT bin (23 Hz) spans several bands */
        f32 slack = freqs[i] < 60 ? 0.6f : 0.85f;
        TEST_CHECK(freqs[i] >= lo * slack && freqs[i] <= hi * (2.0f - slack));
    }

    /* silence decays everything smoothly */
    Spectrum s = {0};
    f32 x[SPECTRUM_FFT];
    for (u32 i = 0; i < SPECTRUM_FFT; i++) x[i] = 0.5f * sinf(2.0f * CORE_PI * 80.0f * (f32)i / 48000.0f);
    for (u32 i = 0; i < 30; i++) spectrum_update(&s, x, SPECTRUM_FFT, 48000, 1.0f / 60.0f);
    TEST_CHECK(s.bass > 0.3f && s.level > 0.3f);
    for (u32 i = 0; i < 180; i++) spectrum_update(&s, 0, 0, 48000, 1.0f / 60.0f);
    f32 mx = s.level + s.bass + s.mid + s.treble + s.beat;
    for (u32 b = 0; b < SPECTRUM_BANDS; b++) mx += s.bands[b];
    TEST_CHECK(mx < 0.01f);

    /* beat detection on a 120 BPM kick pattern */
    {
        static f32 sig[48000 * 6];
        u32 total = CORE_ARRAY_COUNT(sig);
        for (u32 i = 0; i < total; i++) {
            f32 t  = (f32)(i % 24000) / 48000.0f;                  /* kick every 0.5 s */
            f32 k  = expf(-t * 18.0f) * sinf(2.0f * CORE_PI * 55.0f * t);
            f32 hh = 0.05f * sinf((f32)i * 1.7f) * sinf((f32)i * 0.37f); /* some hiss */
            sig[i] = 0.8f * k + hh;
        }
        Spectrum bs = {0};
        u32 beats = 0;
        f32 prev = 0;
        for (u32 end = SPECTRUM_FFT; end <= total; end += 800) { /* 60 fps */
            spectrum_update(&bs, sig + end - SPECTRUM_FFT, SPECTRUM_FFT, 48000, 1.0f / 60.0f);
            if (bs.beat == 1.0f && prev < 1.0f) beats++;
            prev = bs.beat;
        }
        printf("  120 BPM kicks over 6 s -> %u beats\n", beats);
        TEST_CHECK(beats >= 10 && beats <= 13);
    }

    /* cost per call */
    {
        f32 noise[SPECTRUM_FFT];
        u32 seed = 1;
        for (u32 i = 0; i < SPECTRUM_FFT; i++) { seed = seed * 1664525u + 1013904223u; noise[i] = (f32)(seed >> 8) / 16777216.0f - 0.5f; }
        Spectrum ns = {0};
        f64 t0 = platform_time_seconds();
        for (u32 i = 0; i < 1000; i++) spectrum_update(&ns, noise, SPECTRUM_FFT, 48000, 1.0f / 60.0f);
        f64 t1 = platform_time_seconds();
        printf("  spectrum_update: %.1f us per call\n", (t1 - t0) * 1e3);
    }

    /* balance on real music: average band levels over a song (informative) */
    if (test_file_count > 2) {
        Player_Decoder d;
        if (player_dec_open(&d, test_files[2])) {
            static f32 mono[SPECTRUM_FFT * 4], buf[PLAYER_CHUNK_FRAMES * PLAYER_MAX_CHANNELS];
            f32 avg[SPECTRUM_BANDS] = {0};
            Spectrum ms = {0};
            u32 fill = 0, frames = 0, beats = 0;
            f32 prev = 0, lvl = 0;
            player_dec_seek(&d, d.total / 4);
            u64 limit = (u64)d.rate * 60, done = 0;
            while (done < limit) {
                u32 got = player_dec_read(&d, buf, 800);
                if (!got) break;
                done += got;
                memmove(mono, mono + got, sizeof(f32) * (SPECTRUM_FFT - got));
                for (u32 i = 0; i < got; i++) {
                    const f32 *f = buf + i * d.channels;
                    mono[SPECTRUM_FFT - got + i] = d.channels > 1 ? 0.5f * (f[0] + f[1]) : f[0];
                }
                fill += got;
                if (fill < SPECTRUM_FFT) continue;
                spectrum_update(&ms, mono, SPECTRUM_FFT, d.rate, (f32)got / (f32)d.rate);
                for (u32 b = 0; b < SPECTRUM_BANDS; b++) avg[b] += ms.bands[b];
                lvl += ms.level;
                if (ms.beat == 1.0f && prev < 1.0f) beats++;
                prev = ms.beat;
                frames++;
            }
            printf("  %s: mean band levels (low -> high):\n   ", strrchr(test_files[2], '/') + 1);
            for (u32 b = 0; b < SPECTRUM_BANDS; b++) printf(" %.2f", avg[b] / (f32)frames);
            printf("\n  mean level %.2f, %u beats in %.0f s\n", lvl / (f32)frames, beats, (f64)done / d.rate);
            player_dec_close(&d);
        }
    }
}

int main(void) {
    setenv("OFFBEAT_MUTE", "1", 1); /* tests never make sound */

    const char *home = platform_env("HOME");
    char music[4096];
    snprintf(music, sizeof(music), "%s/Music", home ? home : "/tmp");
    platform_walk_dir(music, test_collect, 0);
    printf("found %u mp3 files for testing\n", test_file_count);

    test_spectrum();
    if (test_file_count > 0) {
        test_decoder();
        test_player();
    } else {
        printf("no mp3 files under %s: skipping decoder/player tests\n", music);
    }

    if (test_failures) {
        fprintf(stderr, "player test: %d failure(s)\n", test_failures);
        return 1;
    }
    printf("player test passed\n");
    return 0;
}
