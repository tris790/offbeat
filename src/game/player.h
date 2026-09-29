#ifndef GAME_PLAYER_H
#define GAME_PLAYER_H

#include "../core/types.h"

/*
 * Audio playback engine.
 *
 * A dedicated audio thread owns the decoder (mp3 / flac / ogg) and the
 * platform audio sink. The UI thread only sends small commands and reads a
 * status snapshot, so the UI can never stall audio and vice versa.
 *
 *  - Every command takes effect immediately: play/seek/skip drop the queued
 *    device audio so the response is instant; short gain ramps avoid clicks.
 *  - Pause fades out (~25ms), resume fades in.
 *  - Auto-advance: the UI tells the engine which file comes next
 *    (player_set_next). When the current stream ends the audio thread opens it
 *    itself with no UI round-trip, bumps `advance_count` and reports the new
 *    `track_token`; the UI then updates its queue and sets the following one.
 *  - Position is reported as the *audible* position (sink latency removed)
 *    and interpolated with the clock, so progress bars move smoothly.
 */

typedef struct Player Player;

Player *player_create(void);
void    player_destroy(Player *p);

/* Open `path` and start playing at `start_s`. `track_token` is an opaque id
   echoed back in the status (use the track's path hash). Copies `path`. */
void player_play_file(Player *p, const char *path, u64 track_token,
                      f64 start_s, b32 start_paused);
/* Stop and unload the current stream (and drop `next`). */
void player_stop(Player *p);
/* File to continue with when the current one ends. path=0 clears it. */
void player_set_next(Player *p, const char *path, u64 track_token);
void player_set_paused(Player *p, b32 paused);
void player_seek(Player *p, f64 seconds);
/* 0..1, perceptual (a cubic curve is applied internally). Ramped. */
void player_set_volume(Player *p, f32 volume);

typedef struct {
    u64 track_token;   /* track currently audible (0 = none)                 */
    b32 loaded;        /* a stream is open                                    */
    b32 paused;        /* user-paused (or not started)                        */
    b32 ended;         /* reached end with no `next`; stays paused at the end */
    b32 error;         /* last open failed                                    */
    f64 position_s;    /* audible position, clock-interpolated                */
    f64 duration_s;    /* 0 if unknown                                        */
    u32 sample_rate;
    u32 channels;
    u64 advance_count; /* increments on every automatic advance to `next`     */
    u64 load_count;    /* increments on every successful open                 */
} Player_Status;

Player_Status player_status(Player *p);

/* Copy the most recent `count` mono samples ending at the audible position
   into `out` (for visualization). Returns samples written (0 if nothing is
   playing); pads with zeros when history is short. */
u32 player_vis_samples(Player *p, f32 *out, u32 count);

/* ---- spectrum analysis (UI thread) ---- */

#define SPECTRUM_BANDS 48
#define SPECTRUM_FFT   2048

typedef struct {
    f32 bands[SPECTRUM_BANDS]; /* smoothed, log-spaced 30Hz..16kHz, ~0..1      */
    f32 peaks[SPECTRUM_BANDS]; /* slowly falling peak per band                 */
    f32 bass, mid, treble;     /* smoothed energy per range, ~0..1             */
    f32 level;                 /* overall loudness, ~0..1                      */
    f32 beat;                  /* 1 on a detected kick/onset, decays to 0      */
    f32 bass_avg;              /* internal: running bass average for onsets    */
    u32 sample_rate;
} Spectrum;

/* Analyze the latest samples (from player_vis_samples, SPECTRUM_FFT long).
   `dt` drives the smoothing so it is frame-rate independent. When not
   playing, pass count=0 and everything decays smoothly to zero. */
void spectrum_update(Spectrum *s, const f32 *samples, u32 count, u32 sample_rate, f32 dt);

#endif /* GAME_PLAYER_H */
