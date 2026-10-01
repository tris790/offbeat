/*
 * Audio playback engine (see player.h).
 *
 * Threading: the UI thread posts commands into a small mutex-protected
 * mailbox and reads a published status snapshot; the audio thread ("offbeat-
 * audio") owns the decoder and the sink. The mailbox coalesces: the latest
 * play / seek / pause / volume request wins, so a burst of seeks while the
 * user drags the progress bar costs one seek, and it can never overflow. The
 * mutex is only ever held for a few copies, never across file IO or a
 * blocking sink write.
 *
 * The audio thread feeds the sink in small slices (~5 ms) and re-checks the
 * mailbox between slices, so a command waits at most one slice plus one
 * device period. play/seek drop the queued device audio; short gain ramps
 * hide the cut. When paused (or idle/ended) the thread sleeps on the condvar.
 *
 * Decoders stream from the file: mp3 (dr_mp3), flac (dr_flac), ogg vorbis
 * (stb_vorbis). The libraries are compiled once with gcc -O2 in
 * third_party/impl.c; only their declarations are included here.
 */

#include "player.h"
#include "../core/string.h"
#include "../platform/platform.h"

#include "third_party/dr_mp3.h"
#include "third_party/dr_flac.h"
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#define PLAYER_PATH_MAX        4096
#define PLAYER_CHUNK_FRAMES    1024    /* frames decoded per step                 */
#define PLAYER_WRITE_FRAMES    256     /* frames per sink write (~5 ms)           */
#define PLAYER_MAX_CHANNELS    8       /* flac's maximum                          */
#define PLAYER_LATENCY_S       0.05    /* requested device buffer                 */
#define PLAYER_FADE_IN_S       0.008   /* after open / seek / resume              */
#define PLAYER_FADE_OUT_S      0.025   /* before pausing                          */
#define PLAYER_VOLUME_RAMP_S   0.03    /* full-scale volume slew                  */
#define PLAYER_EXTRAPOLATE_S   0.1     /* max clock extrapolation of the position */
#define PLAYER_PREOPEN_S       5.0     /* open `next` this long before the end    */
#define PLAYER_VIS_SIZE        16384   /* mono history (power of two)             */

/* mp3 seek-table scanner */
#define PLAYER_SCAN_BUF        (64 * 1024)
#define PLAYER_SCAN_LOOKAHEAD  (16 * 1024)  /* minimp3 wants >=16K to sync reliably  */
#define PLAYER_SCAN_STEP       (256 * 1024) /* bytes scanned per incremental step    */
#define PLAYER_SEEK_POINTS     1024         /* cap; spacing doubles when full       */
#define PLAYER_SEEK_SPACING_S  1.0          /* initial seek point spacing            */

/* ---- decoders ---------------------------------------------------------- */

typedef enum {
    PLAYER_CODEC_NONE = 0,
    PLAYER_CODEC_MP3,
    PLAYER_CODEC_FLAC,
    PLAYER_CODEC_OGG,
} Player_Codec;

typedef struct { u64 byte, start; } Player_ScanFrame;

/*
 * MP3 seek table, built by walking frame headers (no synthesis) from a second
 * file handle. dr_mp3's own drmp3_calculate_seek_points reads the file twice
 * and lands one mp3 frame late after a seek (the bit reservoir makes the first
 * discarded frame fail silently); this walks the file once and, for every
 * seek point, replays dr_mp3's post-seek resync to record the exact frame the
 * decoder ends up on. The walk runs incrementally on the audio thread (a few
 * hundred KB per step) so opening a file never waits for it; a seek beyond
 * the scanned region finishes the walk up to the target first.
 */
typedef struct {
    Platform_File file;
    u8       *buf;
    drmp3dec *dec;          /* header walk                                  */
    drmp3dec *sim;          /* replays the resync after a table seek        */
    u64 off;                /* file offset of buf[0]                        */
    u32 pos, have;          /* cursor / valid bytes in buf                  */
    u64 begin, end;         /* audio byte range (tags excluded)             */
    b32 eof;
    u64 running;            /* raw PCM frames walked so far                 */
    u64 interval;           /* PCM frames between seek points               */
    u64 next_target;
    Player_ScanFrame hist[3]; /* last three frames (k-2, k-1, k)            */
    u32 frames;
} Player_Mp3Scan;

typedef struct {
    Player_Codec codec;
    drmp3      *mp3;
    drflac     *flac;
    stb_vorbis *ogg;
    u32 rate;
    u32 channels;           /* channels the read call produces (1..8)       */
    u64 total;              /* audible frames, 0 = unknown                  */
    u64 cursor;             /* audible frame index of the next read         */

    /* mp3 only */
    u32 mp3_delay;          /* encoder delay: raw frames before audible 0   */
    b32 mp3_exact_total;    /* length came from a Xing/Info header          */
    drmp3_seek_point *seek_points;
    u32 seek_count;
    Player_Mp3Scan *scan;   /* non-null while the table is being built      */
} Player_Decoder;

static void player_scan_free(Player_Decoder *d) {
    Player_Mp3Scan *s = d->scan;
    if (!s) return;
    platform_file_close(s->file);
    core_heap_free(s->buf);
    core_heap_free(s->dec);
    core_heap_free(s->sim);
    core_heap_free(s);
    d->scan = 0;
}

static void player_dec_close(Player_Decoder *d) {
    player_scan_free(d);
    if (d->mp3) { drmp3_uninit(d->mp3); core_heap_free(d->mp3); }
    if (d->flac) drflac_close(d->flac);
    if (d->ogg) stb_vorbis_close(d->ogg);
    core_heap_free(d->seek_points);
    memset(d, 0, sizeof(*d));
}

/* Compact + refill the scan buffer. Keeps the last walked frames resident:
   seek-point emission replays decoding from two frames back. */
static void player_scan_refill(Player_Mp3Scan *s) {
    u64 keep = s->off + s->pos;
    for (u32 i = 0; i < 3 && i < s->frames; i++) {
        Player_ScanFrame f = s->hist[3 - 1 - i];
        if (f.byte >= s->off && f.byte < keep) keep = f.byte;
    }
    u32 k = (u32)(keep - s->off);
    memmove(s->buf, s->buf + k, s->have - k);
    s->have -= k;
    s->pos  -= k;
    s->off  += k;

    u64 at   = s->off + s->have;
    u64 want = PLAYER_SCAN_BUF - s->have;
    if (want > s->end - at) want = s->end - at;
    u64 got = want ? platform_file_read_at(s->file, at, s->buf + s->have, want) : 0;
    s->have += (u32)got;
    if (got < want || s->off + s->have >= s->end) s->eof = true;
}

/* Called right after frame k (hist[2]) was walked. Replays what
   drmp3_seek_to_pcm_frame does with a seek point at frame k-2: reset the
   decoder there, then decode until two frames *succeed* (a frame whose bit
   reservoir is missing fails and is skipped silently). The second success is
   left in dr_mp3's frame buffer, so that is the frame the point maps to. */
static void player_scan_emit(Player_Decoder *d, Player_Mp3Scan *s) {
    u64 from = s->hist[0].byte;
    drmp3dec_init(s->sim);
    u32 p = (u32)(from - s->off);
    u32 ok = 0;
    u64 landing = 0;
    while (p < s->have) {
        drmp3dec_frame_info info;
        u32 at = p;
        int n = drmp3dec_decode_frame(s->sim, s->buf + p, (int)(s->have - p), 0, &info);
        if (info.frame_bytes <= 0) return;
        p += (u32)info.frame_bytes;
        if (n > 0 && ++ok == 2) { landing = s->off + at; break; }
    }
    u64 start;
    if      (ok == 2 && landing == s->hist[1].byte) start = s->hist[1].start;
    else if (ok == 2 && landing == s->hist[2].byte) start = s->hist[2].start;
    else return; /* resync runs past frame k: try again at the next frame */

    if (d->seek_count == PLAYER_SEEK_POINTS) {
        /* Full: keep every other point and double the spacing. */
        for (u32 i = 0; i < PLAYER_SEEK_POINTS / 2; i++) d->seek_points[i] = d->seek_points[i * 2 + 1];
        d->seek_count = PLAYER_SEEK_POINTS / 2;
        s->interval *= 2;
    }
    d->seek_points[d->seek_count++] = (drmp3_seek_point){
        .seekPosInBytes = from, .pcmFrameIndex = start,
        .mp3FramesToDiscard = 2, .pcmFramesToDiscard = 0,
    };
    s->next_target = start + s->interval;
    drmp3_bind_seek_table(d->mp3, d->seek_count, d->seek_points);
}

static void player_scan_finish(Player_Decoder *d) {
    Player_Mp3Scan *s = d->scan;
    if (!d->mp3_exact_total) {
        u64 raw = s->running;
        d->total = raw > d->mp3_delay ? raw - d->mp3_delay : 0;
    }
    player_scan_free(d);
}

/* Walk up to `budget` bytes of frame headers. */
static void player_scan_step(Player_Decoder *d, u64 budget) {
    Player_Mp3Scan *s = d->scan;
    if (!s) return;
    u64 stop = s->off + s->pos + budget;
    while (s->off + s->pos < stop) {
        if (s->have - s->pos < PLAYER_SCAN_LOOKAHEAD && !s->eof) player_scan_refill(s);
        if (s->pos >= s->have) { player_scan_finish(d); return; }

        drmp3dec_frame_info info;
        u32 at = s->pos;
        int n = drmp3dec_decode_frame(s->dec, s->buf + at, (int)(s->have - at), 0, &info);
        if (info.frame_bytes <= 0) {
            if (s->eof) { player_scan_finish(d); return; }
            s->pos++; /* no frame in a full window: step over the junk */
            continue;
        }
        s->pos += (u32)info.frame_bytes;
        if (n <= 0) continue;

        s->hist[0] = s->hist[1];
        s->hist[1] = s->hist[2];
        s->hist[2] = (Player_ScanFrame){ s->off + at, s->running };
        s->frames++;
        if (s->frames >= 3 && s->running >= s->next_target) player_scan_emit(d, s);
        s->running += (u64)n;
    }

    /* No Xing/Info header: extrapolate the length from the bytes walked. */
    if (!d->mp3_exact_total) {
        u64 walked = s->off + s->pos - s->begin;
        u64 bytes  = s->end - s->begin;
        if (walked > 0) d->total = (u64)((f64)s->running * (f64)bytes / (f64)walked);
    }
}

/* Make sure the table covers raw frame `raw` (a few frames of margin). */
static void player_scan_until(Player_Decoder *d, u64 raw) {
    while (d->scan && d->scan->running < raw + 4 * DRMP3_MAX_PCM_FRAMES_PER_MP3_FRAME) {
        player_scan_step(d, PLAYER_SCAN_STEP);
    }
}

static b32 player_open_mp3(Player_Decoder *d, const char *path) {
    d->mp3 = core_heap_alloc(sizeof(drmp3));
    if (!d->mp3) return false;
    if (!drmp3_init_file(d->mp3, path, 0)) {
        core_heap_free(d->mp3);
        d->mp3 = 0;
        return false;
    }
    drmp3 *m = d->mp3;
    d->codec     = PLAYER_CODEC_MP3;
    d->rate      = m->sampleRate;
    d->channels  = m->channels;
    d->mp3_delay = m->delayInPCMFrames;
    if (m->totalPCMFrameCount != DRMP3_UINT64_MAX) {
        d->mp3_exact_total = true;
        d->total = drmp3_get_pcm_frame_count(m); /* instant with a Xing header */
    }

    /* Seek table: the walk itself happens incrementally (player_scan_step). */
    Player_Mp3Scan *s = core_heap_calloc(sizeof(*s));
    d->seek_points = core_heap_alloc(PLAYER_SEEK_POINTS * sizeof(drmp3_seek_point));
    if (!s || !d->seek_points) { core_heap_free(s); return true; }
    s->file = platform_file_open_read(path);
    s->buf  = core_heap_alloc(PLAYER_SCAN_BUF);
    s->dec  = core_heap_alloc(sizeof(drmp3dec));
    s->sim  = core_heap_alloc(sizeof(drmp3dec));
    d->scan = s;
    if (!platform_file_valid(s->file) || !s->buf || !s->dec || !s->sim) {
        player_scan_free(d);
        return true; /* plays fine; seeking falls back to dr_mp3's slow path */
    }
    drmp3dec_init(s->dec);
    s->begin = s->off = m->streamStartOffset;
    s->end   = m->streamLength != DRMP3_UINT64_MAX ? m->streamLength : platform_file_size(s->file);
    if (s->end < s->begin) s->end = s->begin;
    s->interval    = (u64)(PLAYER_SEEK_SPACING_S * (f64)d->rate);
    s->next_target = s->interval;
    return true;
}

static b32 player_open_flac(Player_Decoder *d, const char *path) {
    d->flac = drflac_open_file(path, 0);
    if (!d->flac) return false;
    d->codec    = PLAYER_CODEC_FLAC;
    d->rate     = d->flac->sampleRate;
    d->channels = d->flac->channels;
    d->total    = d->flac->totalPCMFrameCount;
    return true;
}

static b32 player_open_ogg(Player_Decoder *d, const char *path) {
    int err = 0;
    d->ogg = stb_vorbis_open_filename(path, &err, 0);
    if (!d->ogg) return false;
    stb_vorbis_info info = stb_vorbis_get_info(d->ogg);
    d->codec    = PLAYER_CODEC_OGG;
    d->rate     = info.sample_rate;
    /* stb_vorbis mixes down to the channel count we ask for. */
    d->channels = info.channels > 2 ? 2 : (u32)info.channels;
    u32 len = stb_vorbis_stream_length_in_samples(d->ogg);
    d->total = (len == 0xFFFFFFFFu) ? 0 : len;
    return true;
}

static b32 player_open_codec(Player_Decoder *d, const char *path, Player_Codec c) {
    switch (c) {
        case PLAYER_CODEC_MP3:  return player_open_mp3(d, path);
        case PLAYER_CODEC_FLAC: return player_open_flac(d, path);
        case PLAYER_CODEC_OGG:  return player_open_ogg(d, path);
        default:                return false;
    }
}

/* Pick the codec by extension; if that fails (or the extension is unknown)
   try the others. mp3 goes last: its resync happily "finds" frames in
   arbitrary data. */
static b32 player_dec_open(Player_Decoder *d, const char *path) {
    memset(d, 0, sizeof(*d));
    Player_Codec first = PLAYER_CODEC_NONE;
    const char *ext = strrchr(path, '.');
    if (ext) {
        if      (core_str_eq_ascii_ci(core_str(ext), core_str_lit(".mp3")))                             first = PLAYER_CODEC_MP3;
        else if (core_str_eq_ascii_ci(core_str(ext), core_str_lit(".flac")))                            first = PLAYER_CODEC_FLAC;
        else if (core_str_eq_ascii_ci(core_str(ext), core_str_lit(".ogg")) || core_str_eq_ascii_ci(core_str(ext), core_str_lit(".oga"))) first = PLAYER_CODEC_OGG;
    }
    Player_Codec order[4] = { first, PLAYER_CODEC_FLAC, PLAYER_CODEC_OGG, PLAYER_CODEC_MP3 };
    for (u32 i = 0; i < 4; i++) {
        if (order[i] == PLAYER_CODEC_NONE || (i > 0 && order[i] == first)) continue;
        if (!player_open_codec(d, path, order[i])) { player_dec_close(d); continue; }
        if (d->rate >= 1000 && d->rate <= 768000 &&
            d->channels >= 1 && d->channels <= PLAYER_MAX_CHANNELS) return true;
        player_dec_close(d);
    }
    return false;
}

/* Interleaved f32, `d->channels` per frame. Returns 0 at end of stream. */
static u32 player_dec_read(Player_Decoder *d, f32 *out, u32 frames) {
    u64 got = 0;
    switch (d->codec) {
        case PLAYER_CODEC_MP3:  got = drmp3_read_pcm_frames_f32(d->mp3, frames, out); break;
        case PLAYER_CODEC_FLAC: got = drflac_read_pcm_frames_f32(d->flac, frames, out); break;
        case PLAYER_CODEC_OGG: {
            int n = stb_vorbis_get_samples_float_interleaved(d->ogg, (int)d->channels, out,
                                                             (int)(frames * d->channels));
            got = n > 0 ? (u64)n : 0;
        } break;
        default: break;
    }
    d->cursor += got;
    return (u32)got;
}

static void player_dec_seek(Player_Decoder *d, u64 frame) {
    if (d->total && frame > d->total) frame = d->total;
    switch (d->codec) {
        case PLAYER_CODEC_MP3: {
            drmp3 *m = d->mp3;
            u64 raw = frame + d->mp3_delay;
            player_scan_until(d, raw);
            if (d->seek_count == 0 || raw < d->seek_points[0].pcmFrameIndex) {
                /* Before the first seek point: decode forward from the start.
                   (dr_mp3's own path would skip the encoder delay twice.) */
                drmp3_seek_to_pcm_frame(m, 0);
                if (frame) drmp3_read_pcm_frames_s16(m, frame, 0);
            } else {
                drmp3_seek_to_pcm_frame(m, raw); /* false past the end: cursor stops there */
            }
            d->cursor = m->currentPCMFrame > d->mp3_delay ? m->currentPCMFrame - d->mp3_delay : 0;
        } break;
        case PLAYER_CODEC_FLAC:
            drflac_seek_to_pcm_frame(d->flac, frame);
            d->cursor = d->flac->currentPCMFrame;
            break;
        case PLAYER_CODEC_OGG:
            if (!stb_vorbis_seek(d->ogg, (unsigned)frame)) {
                stb_vorbis_seek_start(d->ogg);
                frame = 0;
            }
            d->cursor = frame;
            break;
        default: break;
    }
}

/* ---- player ------------------------------------------------------------ */

typedef enum {
    PLAYER_RUN_IDLE = 0,  /* nothing flowing (no stream / paused / ended): sleep */
    PLAYER_RUN_PLAYING,   /* decoding and feeding the sink                       */
    PLAYER_RUN_PAUSING,   /* fading out; then drain and go idle                  */
} Player_Run;

typedef struct {
    b32 play;  u64 play_token; f64 play_start; b32 play_paused;
    b32 seek;  f64 seek_s;
    b32 pause; b32 pause_value;
    b32 stop;
    f32 volume;
} Player_Cmd;

struct Player {
    Platform_Thread *thread;
    Platform_Mutex   mutex;
    Platform_Cond    cond;

    /* ---- mailbox: UI -> audio thread (mutex) ---- */
    b32  quit;
    b32  cmd_play;
    char play_path[PLAYER_PATH_MAX];
    u64  play_token;
    f64  play_start;
    b32  play_paused;
    b32  cmd_seek;
    f64  seek_s;
    b32  cmd_pause;
    b32  pause_value;
    b32  cmd_volume;
    f32  volume;
    b32  cmd_stop;
    b32  has_next;
    char next_path[PLAYER_PATH_MAX];
    u64  next_token;
    u32  next_serial;        /* bumped by every player_set_next               */

    /* ---- published: audio thread -> UI (mutex) ---- */
    Player_Status status;    /* position_s = last measurement                 */
    f64  pos_stamp;          /* when position_s was measured                  */
    b32  pos_running;        /* audio is flowing: extrapolate from the stamp  */
    u64  vis_at;             /* vis ring index audible at pos_stamp           */
    u32  gen;                /* bumped by play/seek: backward jumps allowed   */
    f64  shown_pos;          /* last position player_status handed out       */
    u64  shown_token;
    u32  shown_gen;
    /* measurements (tests / diagnostics) */
    f64  dbg_cmd_time;       /* when the last play/seek/resume was requested  */
    f64  dbg_cmd_latency;    /* request -> first audio handed to the sink     */
    f64  dbg_open_s;         /* last decoder open                             */

    /* ---- visualization history: audio thread writes, UI reads lock-free ---- */
    _Atomic u64 vis_total;   /* mono samples ever written                     */
    f32  vis[PLAYER_VIS_SIZE];

    /* ---- audio thread only ---- */
    Player_Decoder  dec;
    Player_Decoder  next_dec;        /* `next`, pre-opened near the end        */
    u32             next_dec_serial; /* serial it was opened for (~0 = none)   */
    Platform_Audio *sink;
    Player_Run      run;
    b32             ended;
    b32             want_mark;       /* record dbg_cmd_latency at next write  */
    char            path[PLAYER_PATH_MAX];
    char            next_open_path[PLAYER_PATH_MAX];
    f32 gain, gain_target, gain_step; /* volume (already cubed)               */
    f32 fade, fade_target, fade_step; /* click-free start / stop              */
    u32 pcm_pos, pcm_len;             /* decoded stereo frames not yet written */
    f32 in[PLAYER_CHUNK_FRAMES * PLAYER_MAX_CHANNELS];
    f32 pcm[PLAYER_CHUNK_FRAMES * 2];
    f32 out[PLAYER_WRITE_FRAMES * 2];
};

static f32 player_rate_step(f64 seconds, u32 rate) {
    return (f32)(1.0 / (seconds * (f64)(rate ? rate : 48000)));
}

/* (Re)open the sink at `rate`; the sink is kept across tracks. */
static b32 player_sink_for(Player *p, u32 rate) {
    if (p->sink && platform_audio_rate(p->sink) != rate) {
        platform_audio_close(p->sink);
        p->sink = 0;
    }
    if (!p->sink) p->sink = platform_audio_open(rate, 2, PLAYER_LATENCY_S);
    return p->sink != 0;
}

/* Publish the audible position. Skipped while a newer play/seek is queued so
   the UI never sees a stale position after it asked for a new one. */
static void player_publish(Player *p) {
    Player_Decoder *d = &p->dec;
    u32 delay   = p->sink ? platform_audio_delay(p->sink) : 0;
    u64 pending = p->pcm_len - p->pcm_pos;
    u64 behind  = pending + delay;
    u64 audible = d->cursor > behind ? d->cursor - behind : 0;
    u64 total   = atomic_load(&p->vis_total);
    u64 vis_at  = total > behind ? total - behind : 0;
    f64 now     = platform_time_seconds();

    platform_mutex_lock(&p->mutex);
    if (!p->cmd_play && !p->cmd_seek && d->rate) {
        p->status.position_s = (f64)audible / (f64)d->rate;
        p->status.duration_s = (f64)d->total / (f64)d->rate;
        p->pos_stamp   = now;
        p->pos_running = p->run != PLAYER_RUN_IDLE;
        p->vis_at      = vis_at;
    }
    platform_mutex_unlock(&p->mutex);
}

static void player_publish_loaded(Player *p) {
    Player_Status *s = &p->status;
    s->loaded      = true;
    s->error       = false;
    s->ended       = false;
    s->sample_rate = p->dec.rate;
    s->channels    = p->dec.channels;
    s->duration_s  = (f64)p->dec.total / (f64)p->dec.rate;
    s->position_s  = (f64)p->dec.cursor / (f64)p->dec.rate;
    p->pos_stamp   = platform_time_seconds();
    p->pos_running = false;
    p->vis_at      = atomic_load(&p->vis_total);
}

static void player_do_open(Player *p, u64 token, f64 start_s, b32 paused) {
    f64 t0 = platform_time_seconds();
    if (p->sink) platform_audio_drop(p->sink); /* silence now, not after the open */
    player_dec_close(&p->dec);
    p->pcm_pos = p->pcm_len = 0;
    p->run   = PLAYER_RUN_IDLE;
    p->ended = false;

    b32 ok = player_dec_open(&p->dec, p->path);
    f64 t1 = platform_time_seconds();
    if (ok) ok = player_sink_for(p, p->dec.rate);
    if (!ok) {
        player_dec_close(&p->dec);
        platform_mutex_lock(&p->mutex);
        if (!p->cmd_play) {
            p->status.loaded     = false;
            p->status.error      = true;
            p->status.paused     = true;
            p->status.duration_s = 0;
            p->status.position_s = 0;
            p->pos_running       = false;
        }
        platform_mutex_unlock(&p->mutex);
        return;
    }

    if (start_s > 0) player_dec_seek(&p->dec, (u64)(start_s * (f64)p->dec.rate));
    p->gain_step   = player_rate_step(PLAYER_VOLUME_RAMP_S, p->dec.rate);
    p->fade        = 0;
    p->fade_target = 1;
    p->fade_step   = player_rate_step(PLAYER_FADE_IN_S, p->dec.rate);
    p->run         = paused ? PLAYER_RUN_IDLE : PLAYER_RUN_PLAYING;
    p->want_mark   = !paused;

    platform_mutex_lock(&p->mutex);
    p->dbg_open_s = t1 - t0;
    p->status.load_count++;
    if (!p->cmd_play) {
        p->status.track_token = token;
        player_publish_loaded(p);
    }
    platform_mutex_unlock(&p->mutex);
}

static void player_do_stop(Player *p) {
    if (p->sink) platform_audio_drop(p->sink);
    player_dec_close(&p->dec);
    p->pcm_pos = p->pcm_len = 0;
    p->run   = PLAYER_RUN_IDLE;
    p->ended = false;

    platform_mutex_lock(&p->mutex);
    if (!p->cmd_play) {
        p->status.track_token = 0;
        p->status.loaded      = false;
        p->status.paused      = true;
        p->status.ended       = false;
        p->status.error       = false;
        p->status.position_s  = 0;
        p->status.duration_s  = 0;
        p->pos_running        = false;
        p->gen++;
    }
    platform_mutex_unlock(&p->mutex);
}

static void player_do_seek(Player *p, f64 seconds) {
    Player_Decoder *d = &p->dec;
    if (!d->codec) return;
    u64 frame = seconds > 0 ? (u64)(seconds * (f64)d->rate) : 0;
    if (d->total && frame > d->total) frame = d->total;
    if (p->sink) platform_audio_drop(p->sink);
    p->pcm_pos = p->pcm_len = 0;
    player_dec_seek(d, frame);

    p->ended = false;
    if (p->run == PLAYER_RUN_PAUSING) p->run = PLAYER_RUN_IDLE; /* the drop silenced it */
    p->fade        = 0;
    p->fade_target = 1;
    p->fade_step   = player_rate_step(PLAYER_FADE_IN_S, d->rate);
    p->want_mark   = p->run == PLAYER_RUN_PLAYING;

    platform_mutex_lock(&p->mutex);
    if (!p->cmd_play && !p->cmd_seek) {
        p->status.ended      = false;
        p->status.position_s = (f64)d->cursor / (f64)d->rate;
        p->pos_stamp   = platform_time_seconds();
        p->pos_running = false;
        p->vis_at      = atomic_load(&p->vis_total);
    }
    platform_mutex_unlock(&p->mutex);
}

static void player_do_pause(Player *p, b32 paused) {
    if (paused) {
        if (p->run == PLAYER_RUN_PLAYING) {
            p->run         = PLAYER_RUN_PAUSING;
            p->fade_target = 0;
            p->fade_step   = player_rate_step(PLAYER_FADE_OUT_S, p->dec.rate);
        }
        return;
    }
    if (!p->dec.codec) return;
    if (p->ended) player_do_seek(p, 0); /* play after the end restarts the track */
    if (p->run != PLAYER_RUN_PLAYING) {
        p->run         = PLAYER_RUN_PLAYING;
        p->fade_target = 1;
        p->fade_step   = player_rate_step(PLAYER_FADE_IN_S, p->dec.rate);
        p->want_mark   = true;
    }
}

/* Decode one chunk into p->pcm (stereo) and the visualization ring.
   Returns frames decoded; 0 at end of stream. */
static u32 player_fill(Player *p) {
    Player_Decoder *d = &p->dec;
    u32 got = player_dec_read(d, p->in, PLAYER_CHUNK_FRAMES);
    u32 ch  = d->channels;
    u64 base = atomic_load(&p->vis_total);
    for (u32 i = 0; i < got; i++) {
        const f32 *f = p->in + (u64)i * ch;
        f32 l = f[0];
        f32 r = ch > 1 ? f[1] : l; /* mono -> both; >2 channels -> first two */
        p->pcm[i * 2 + 0] = l;
        p->pcm[i * 2 + 1] = r;
        p->vis[(base + i) & (PLAYER_VIS_SIZE - 1)] = 0.5f * (l + r);
    }
    atomic_store(&p->vis_total, base + got);
    p->pcm_pos = 0;
    p->pcm_len = got;
    return got;
}

/* Apply volume + fade ramps to up to `n` pending frames into p->out. While
   pausing, stops at the frame where the fade reaches silence so nothing
   after it is consumed. Returns frames produced. */
static u32 player_mix(Player *p, u32 n) {
    const f32 *src = p->pcm + (u64)p->pcm_pos * 2;
    f32 *dst = p->out;
    f32 g = p->gain, gt = p->gain_target, gs = p->gain_step;
    f32 f = p->fade, ft = p->fade_target, fs = p->fade_step;
    b32 pausing = p->run == PLAYER_RUN_PAUSING;
    u32 i = 0;
    while (i < n) {
        if      (g < gt) { g += gs; if (g > gt) g = gt; }
        else if (g > gt) { g -= gs; if (g < gt) g = gt; }
        if      (f < ft) { f += fs; if (f > ft) f = ft; }
        else if (f > ft) { f -= fs; if (f < ft) f = ft; }
        f32 k = g * f * f * (3.0f - 2.0f * f); /* smoothstep fade */
        dst[i * 2 + 0] = src[i * 2 + 0] * k;
        dst[i * 2 + 1] = src[i * 2 + 1] * k;
        i++;
        if (pausing && f <= 0) break;
    }
    p->gain = g;
    p->fade = f;
    return i;
}

/* Open `next` ahead of time so the switch at the end costs nothing. */
static void player_preopen_next(Player *p) {
    Player_Decoder *d = &p->dec;
    if (!d->total || (f64)d->cursor + PLAYER_PREOPEN_S * (f64)d->rate < (f64)d->total) return;

    platform_mutex_lock(&p->mutex);
    b32 has    = p->has_next;
    u32 serial = p->next_serial;
    b32 want   = has && serial != p->next_dec_serial;
    if (want) core_cstr_copy(p->next_open_path, PLAYER_PATH_MAX, p->next_path);
    platform_mutex_unlock(&p->mutex);
    if (!want) return;

    player_dec_close(&p->next_dec);
    p->next_dec_serial = serial;
    if (player_dec_open(&p->next_dec, p->next_open_path)) player_scan_step(&p->next_dec, PLAYER_SCAN_STEP);
}

static void player_end_of_stream(Player *p) {
    platform_mutex_lock(&p->mutex);
    b32 has    = p->has_next;
    u64 token  = p->next_token;
    u32 serial = p->next_serial;
    b32 ready  = has && p->next_dec.codec && p->next_dec_serial == serial;
    if (has && !ready) core_cstr_copy(p->next_open_path, PLAYER_PATH_MAX, p->next_path);
    p->has_next = false; /* consumed: the UI sets the following one */
    platform_mutex_unlock(&p->mutex);

    Player_Decoder nd = {0};
    b32 opened = false;
    if (ready) {
        nd = p->next_dec;
        memset(&p->next_dec, 0, sizeof(p->next_dec));
        opened = true;
    } else if (has) {
        opened = player_dec_open(&nd, p->next_open_path);
    }
    p->next_dec_serial = ~0u;

    if (opened) {
        if (!p->sink || platform_audio_rate(p->sink) != nd.rate) {
            if (p->sink) platform_audio_drain(p->sink); /* let the old tail play first */
        }
        player_dec_close(&p->dec);
        p->dec = nd;
        if (player_sink_for(p, nd.rate)) {
            core_cstr_copy(p->path, PLAYER_PATH_MAX, p->next_open_path);
            p->gain_step = player_rate_step(PLAYER_VOLUME_RAMP_S, nd.rate);
            platform_mutex_lock(&p->mutex);
            p->status.advance_count++;
            p->status.load_count++;
            if (!p->cmd_play) {
                p->status.track_token = token;
                player_publish_loaded(p);
                p->pos_running = true;
                p->gen++;
            }
            platform_mutex_unlock(&p->mutex);
            return;
        }
        player_dec_close(&p->dec);
    }

    /* Nothing to continue with: let the tail play out and stop at the end. */
    if (p->sink) platform_audio_drain(p->sink);
    p->run   = PLAYER_RUN_IDLE;
    p->ended = p->dec.codec != PLAYER_CODEC_NONE;
    platform_mutex_lock(&p->mutex);
    if (!p->cmd_play && !p->cmd_seek) {
        p->status.ended  = true;
        p->status.paused = true;
        p->status.error  = has; /* `next` failed to open */
        if (!p->dec.codec) p->status.loaded = false;
        if (p->status.duration_s > 0) p->status.position_s = p->status.duration_s;
        p->pos_stamp   = platform_time_seconds();
        p->pos_running = false;
    }
    platform_mutex_unlock(&p->mutex);
}

/* Feed one slice (~5 ms) to the sink. */
static void player_step(Player *p) {
    if (p->dec.scan) player_scan_step(&p->dec, PLAYER_SCAN_STEP);
    player_preopen_next(p);

    if (p->pcm_pos == p->pcm_len && player_fill(p) == 0) {
        player_end_of_stream(p);
        return;
    }
    u32 n = p->pcm_len - p->pcm_pos;
    if (n > PLAYER_WRITE_FRAMES) n = PLAYER_WRITE_FRAMES;
    n = player_mix(p, n);
    platform_audio_write(p->sink, p->out, n);
    p->pcm_pos += n;

    if (p->want_mark) {
        p->want_mark = false;
        f64 now = platform_time_seconds();
        platform_mutex_lock(&p->mutex);
        p->dbg_cmd_latency = now - p->dbg_cmd_time;
        platform_mutex_unlock(&p->mutex);
    }
    if (p->run == PLAYER_RUN_PAUSING && p->fade <= 0) {
        platform_audio_drain(p->sink); /* the faded tail plays out */
        p->run = PLAYER_RUN_IDLE;
    }
    player_publish(p);
}

static b32 player_cmd_pending(Player *p) {
    return p->cmd_play || p->cmd_seek || p->cmd_pause || p->cmd_volume || p->cmd_stop;
}

static void player_thread(void *user) {
    Player *p = user;
    for (;;) {
        Player_Cmd c = {0};
        platform_mutex_lock(&p->mutex);
        while (!p->quit && !player_cmd_pending(p) && p->run == PLAYER_RUN_IDLE && !p->dec.scan) {
            platform_cond_wait(&p->cond, &p->mutex);
        }
        b32 quit = p->quit;
        if (p->cmd_play) {
            c.play        = true;
            c.play_token  = p->play_token;
            c.play_start  = p->play_start;
            c.play_paused = p->play_paused;
            core_cstr_copy(p->path, PLAYER_PATH_MAX, p->play_path);
        }
        c.stop        = p->cmd_stop;
        c.seek        = p->cmd_seek;
        c.seek_s      = p->seek_s;
        c.pause       = p->cmd_pause;
        c.pause_value = p->pause_value;
        c.volume      = p->volume;
        p->cmd_play = p->cmd_seek = p->cmd_pause = p->cmd_volume = p->cmd_stop = false;
        platform_mutex_unlock(&p->mutex);
        if (quit) break;

        p->gain_target = c.volume * c.volume * c.volume;
        if (p->run == PLAYER_RUN_IDLE) p->gain = p->gain_target; /* silent: no ramp needed */
        if (c.stop)  player_do_stop(p);
        if (c.play)  player_do_open(p, c.play_token, c.play_start, c.play_paused);
        if (c.seek)  player_do_seek(p, c.seek_s);
        if (c.pause) player_do_pause(p, c.pause_value);

        if (p->run != PLAYER_RUN_IDLE) {
            player_step(p);
        } else if (p->dec.scan) {
            /* Idle but the seek table is incomplete: keep walking so seeks
               while paused stay instant. */
            player_scan_step(&p->dec, PLAYER_SCAN_STEP);
            player_publish(p);
        }
    }

    player_dec_close(&p->dec);
    player_dec_close(&p->next_dec);
    if (p->sink) platform_audio_close(p->sink);
    p->sink = 0;
}

/* ---- public API (UI thread) ---- */

Player *player_create(void) {
    Player *p = core_heap_calloc(sizeof(Player));
    if (!p) return 0;
    platform_mutex_init(&p->mutex);
    platform_cond_init(&p->cond);
    p->volume          = 1.0f;
    p->gain            = 1.0f;
    p->gain_target     = 1.0f;
    p->next_dec_serial = ~0u;
    p->status.paused   = true;
    p->thread = platform_thread_start(player_thread, p, "offbeat-audio");
    if (!p->thread) { core_heap_free(p); return 0; }
    return p;
}

void player_destroy(Player *p) {
    if (!p) return;
    platform_mutex_lock(&p->mutex);
    p->quit = true;
    platform_cond_signal(&p->cond);
    platform_mutex_unlock(&p->mutex);
    platform_thread_join(p->thread);
    core_heap_free(p);
}

void player_play_file(Player *p, const char *path, u64 track_token, f64 start_s, b32 start_paused) {
    if (!p || !path) return;
    platform_mutex_lock(&p->mutex);
    core_cstr_copy(p->play_path, PLAYER_PATH_MAX, path);
    p->cmd_play    = true;
    p->play_token  = track_token;
    p->play_start  = start_s > 0 ? start_s : 0;
    p->play_paused = start_paused;
    p->cmd_seek    = false; /* superseded */
    p->cmd_pause   = false;
    /* Reflect the request right away; the audio thread fills in the rest. */
    p->status.track_token = track_token;
    p->status.paused      = start_paused;
    p->status.ended       = false;
    p->status.error       = false;
    p->status.position_s  = p->play_start;
    p->status.duration_s  = 0;
    p->pos_stamp    = platform_time_seconds();
    p->pos_running  = false;
    p->dbg_cmd_time = p->pos_stamp;
    p->gen++;
    platform_cond_signal(&p->cond);
    platform_mutex_unlock(&p->mutex);
}

void player_stop(Player *p) {
    if (!p) return;
    platform_mutex_lock(&p->mutex);
    p->cmd_stop  = true;
    p->cmd_play  = false; /* superseded */
    p->cmd_seek  = false;
    p->cmd_pause = false;
    p->has_next  = false;
    p->next_serial++;
    p->status.track_token = 0;
    p->status.loaded      = false;
    p->status.paused      = true;
    p->status.ended       = false;
    p->status.error       = false;
    p->status.position_s  = 0;
    p->status.duration_s  = 0;
    p->pos_running        = false;
    p->gen++;
    platform_cond_signal(&p->cond);
    platform_mutex_unlock(&p->mutex);
}

void player_set_next(Player *p, const char *path, u64 track_token) {
    if (!p) return;
    platform_mutex_lock(&p->mutex);
    p->has_next = path != 0;
    if (path) core_cstr_copy(p->next_path, PLAYER_PATH_MAX, path);
    p->next_token = path ? track_token : 0;
    p->next_serial++;
    platform_mutex_unlock(&p->mutex);
}

void player_set_paused(Player *p, b32 paused) {
    if (!p) return;
    platform_mutex_lock(&p->mutex);
    p->cmd_pause   = true;
    p->pause_value = paused;
    if (p->status.loaded || p->cmd_play) {
        if (!paused && p->status.ended) {
            /* resuming after the end restarts the track */
            p->status.ended      = false;
            p->status.position_s = 0;
            p->pos_stamp         = platform_time_seconds();
            p->gen++;
        }
        p->status.paused = paused;
        if (!paused) p->dbg_cmd_time = platform_time_seconds();
    }
    platform_cond_signal(&p->cond);
    platform_mutex_unlock(&p->mutex);
}

void player_seek(Player *p, f64 seconds) {
    if (!p) return;
    platform_mutex_lock(&p->mutex);
    if (!p->status.loaded && !p->cmd_play) { platform_mutex_unlock(&p->mutex); return; }
    if (seconds < 0) seconds = 0;
    if (p->status.duration_s > 0 && seconds > p->status.duration_s) seconds = p->status.duration_s;
    p->cmd_seek = true;
    p->seek_s   = seconds;
    p->status.position_s = seconds;
    p->status.ended      = false;
    p->pos_stamp    = platform_time_seconds();
    p->dbg_cmd_time = p->pos_stamp;
    p->gen++;
    platform_cond_signal(&p->cond);
    platform_mutex_unlock(&p->mutex);
}

void player_set_volume(Player *p, f32 volume) {
    if (!p) return;
    if (!(volume >= 0)) volume = 0; /* also catches NaN */
    if (volume > 1) volume = 1;
    platform_mutex_lock(&p->mutex);
    if (volume != p->volume) {
        p->volume     = volume;
        p->cmd_volume = true;
        platform_cond_signal(&p->cond);
    }
    platform_mutex_unlock(&p->mutex);
}

Player_Status player_status(Player *p) {
    Player_Status s = {0};
    if (!p) return s;
    f64 now = platform_time_seconds();
    platform_mutex_lock(&p->mutex);
    s = p->status;
    f64 pos = s.position_s;
    if (p->pos_running) {
        f64 e = now - p->pos_stamp;
        if (e > PLAYER_EXTRAPOLATE_S) e = PLAYER_EXTRAPOLATE_S;
        if (e > 0) pos += e;
    }
    if (s.duration_s > 0 && pos > s.duration_s) pos = s.duration_s;
    if (pos < 0) pos = 0;
    /* Smoothness: within one track and without a seek in between, never step
       backwards (device-delay jitter, extrapolation overshoot); hold instead. */
    if (s.track_token == p->shown_token && p->gen == p->shown_gen &&
        pos < p->shown_pos && p->shown_pos - pos < 0.5) {
        pos = p->shown_pos;
    }
    p->shown_pos   = pos;
    p->shown_token = s.track_token;
    p->shown_gen   = p->gen;
    platform_mutex_unlock(&p->mutex);
    s.position_s = pos;
    return s;
}

u32 player_vis_samples(Player *p, f32 *out, u32 count) {
    if (!p || !out || count == 0) return 0;
    platform_mutex_lock(&p->mutex);
    b32 running = p->pos_running;
    u64 at      = p->vis_at;
    f64 stamp   = p->pos_stamp;
    u32 rate    = p->status.sample_rate;
    platform_mutex_unlock(&p->mutex);
    if (!running || !rate) return 0;

    f64 e = platform_time_seconds() - stamp;
    if (e < 0) e = 0;
    if (e > PLAYER_EXTRAPOLATE_S) e = PLAYER_EXTRAPOLATE_S;
    u64 end   = at + (u64)(e * (f64)rate);
    u64 total = atomic_load(&p->vis_total);
    if (end > total) end = total;
    /* Oldest index still safe to read while the writer adds up to a chunk. */
    u64 oldest = total > PLAYER_VIS_SIZE - PLAYER_CHUNK_FRAMES ? total - (PLAYER_VIS_SIZE - PLAYER_CHUNK_FRAMES) : 0;
    for (u32 i = 0; i < count; i++) {
        u64 back = (u64)(count - i);
        if (back > end || end - back < oldest) { out[i] = 0; continue; }
        out[i] = p->vis[(end - back) & (PLAYER_VIS_SIZE - 1)];
    }
    return count;
}
