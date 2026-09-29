/*
 * ALSA part of the Linux platform layer: the blocking PCM sink used by the
 * audio thread (see the "audio output" section of platform.h).
 *
 * We always talk to the "default" device, which on a modern desktop routes
 * through PipeWire / PulseAudio, so the soft resampler rarely has work to do.
 * Samples are interleaved float32.
 *
 * With OFFBEAT_MUTE set, ALSA is never touched: a null sink consumes frames at
 * the wall-clock rate through a virtual queue, so timing, positions and delay
 * behave like a real device (headless screenshots and tests).
 */

#include "platform.h"

#include <alsa/asoundlib.h>
#include <errno.h>

struct Platform_Audio {
    snd_pcm_t *pcm;          /* 0 for the muted sink          */
    u32        rate;
    u32        channels;
    b32        broken;       /* device vanished: behave muted */

    /* Muted sink: a virtual device queue drained by the monotonic clock. */
    f64 mute_queued;         /* frames not yet "played"       */
    f64 mute_capacity;       /* frames the buffer can hold    */
    f64 mute_stamp;          /* last time the queue was aged  */
};

/* ---- muted sink ---- */

static void alsa_mute_age(Platform_Audio *a) {
    f64 now = platform_time_seconds();
    a->mute_queued -= (now - a->mute_stamp) * (f64)a->rate;
    if (a->mute_queued < 0) a->mute_queued = 0;
    a->mute_stamp = now;
}

static u32 alsa_mute_write(Platform_Audio *a, u32 frame_count) {
    alsa_mute_age(a);
    /* Block like a full device would: wait until the new frames fit. */
    f64 overflow = a->mute_queued + (f64)frame_count - a->mute_capacity;
    if (overflow > 0) {
        platform_sleep(overflow / (f64)a->rate);
        alsa_mute_age(a);
    }
    a->mute_queued += (f64)frame_count;
    return frame_count;
}

/* ---- lifecycle ---- */

Platform_Audio *platform_audio_open(u32 sample_rate, u32 channels, f64 latency_s) {
    if (sample_rate == 0 || channels == 0) return 0;
    if (latency_s < 0.01) latency_s = 0.01;

    Platform_Audio *a = core_heap_calloc(sizeof(*a));
    if (!a) return 0;
    a->rate     = sample_rate;
    a->channels = channels;

    if (platform_env("OFFBEAT_MUTE")) {
        a->mute_capacity = latency_s * (f64)sample_rate;
        a->mute_stamp    = platform_time_seconds();
        return a;
    }

    snd_pcm_t *pcm = 0;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        core_heap_free(a);
        return 0;
    }
    /* soft_resample=1: let alsa-lib convert if the device can't run the
       stream's native rate. */
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           channels, sample_rate, 1, (unsigned)(latency_s * 1e6)) < 0) {
        snd_pcm_close(pcm);
        core_heap_free(a);
        return 0;
    }

    /* snd_pcm_set_params only starts the stream once the whole buffer is full.
       Start after one period instead, so a short tail (end of track, a seek
       near the end) still plays out and drain/delay never stall on it. */
    snd_pcm_uframes_t buffer_size = 0, period_size = 0;
    if (snd_pcm_get_params(pcm, &buffer_size, &period_size) == 0 && period_size > 0) {
        snd_pcm_sw_params_t *sw;
        snd_pcm_sw_params_alloca(&sw);
        if (snd_pcm_sw_params_current(pcm, sw) == 0) {
            snd_pcm_sw_params_set_start_threshold(pcm, sw, period_size);
            snd_pcm_sw_params(pcm, sw);
        }
    }

    a->pcm = pcm;
    return a;
}

void platform_audio_close(Platform_Audio *a) {
    if (!a) return;
    if (a->pcm) snd_pcm_close(a->pcm); /* drops whatever is still queued */
    core_heap_free(a);
}

/* ---- streaming ---- */

u32 platform_audio_write(Platform_Audio *a, const f32 *frames, u32 frame_count) {
    if (!a || frame_count == 0) return 0;
    if (!a->pcm || a->broken) return alsa_mute_write(a, frame_count);

    u32 done = 0;
    while (done < frame_count) {
        snd_pcm_sframes_t n = snd_pcm_writei(a->pcm, frames + (u64)done * a->channels,
                                             frame_count - done);
        if (n >= 0) { done += (u32)n; continue; }
        if (n == -EAGAIN) continue;
        /* -EPIPE (underrun) / -ESTRPIPE (suspend): recover and retry. */
        if (snd_pcm_recover(a->pcm, (int)n, 1) < 0) {
            /* The device is gone (unplugged, server died). Keep the audio
               thread's clock ticking instead of spinning on errors. */
            a->broken        = true;
            a->mute_capacity = 0.05 * (f64)a->rate;
            a->mute_queued   = 0;
            a->mute_stamp    = platform_time_seconds();
            return done + alsa_mute_write(a, frame_count - done);
        }
    }
    return done;
}

u32 platform_audio_delay(Platform_Audio *a) {
    if (!a) return 0;
    if (!a->pcm || a->broken) {
        alsa_mute_age(a);
        return (u32)a->mute_queued;
    }
    snd_pcm_sframes_t d = 0;
    if (snd_pcm_delay(a->pcm, &d) < 0 || d < 0) return 0;
    return (u32)d;
}

void platform_audio_drop(Platform_Audio *a) {
    if (!a) return;
    if (!a->pcm || a->broken) { a->mute_queued = 0; a->mute_stamp = platform_time_seconds(); return; }
    snd_pcm_drop(a->pcm);
    snd_pcm_prepare(a->pcm);
}

void platform_audio_drain(Platform_Audio *a) {
    if (!a) return;
    if (!a->pcm || a->broken) {
        alsa_mute_age(a);
        platform_sleep(a->mute_queued / (f64)a->rate);
        a->mute_queued = 0;
        a->mute_stamp  = platform_time_seconds();
        return;
    }
    /* drain leaves the PCM in SETUP state; prepare so the next write works. */
    snd_pcm_drain(a->pcm);
    snd_pcm_prepare(a->pcm);
}

u32 platform_audio_rate(Platform_Audio *a)     { return a ? a->rate : 0; }
u32 platform_audio_channels(Platform_Audio *a) { return a ? a->channels : 0; }
