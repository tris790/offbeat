/*
 * Spectrum analysis for the visualizers (UI thread; see player.h).
 *
 * Hann window -> 2048-point real FFT (computed as a 1024-point complex FFT of
 * the even/odd samples plus a split step) -> log-spaced bands in dB ->
 * frame-rate independent smoothing. Tables are built once into static
 * arrays; a call costs a few tens of microseconds.
 */

#include "player.h"

#include <math.h>
#include <string.h>

#define SPECTRUM_HALF    (SPECTRUM_FFT / 2)  /* complex FFT size / bins kept */
#define SPECTRUM_LO_HZ   30.0f
#define SPECTRUM_HI_HZ   16000.0f
#define SPECTRUM_FLOOR_DB (-60.0f)           /* maps to 0; 0 dB maps to 1    */
#define SPECTRUM_TILT_DB 3.0f                /* per octave, pivot at 1 kHz    */

static b32 spectrum_ready;
static f32 spectrum_window[SPECTRUM_FFT];
static f32 spectrum_cos[SPECTRUM_HALF];      /* cos(2*pi*k/N), k < N/2       */
static f32 spectrum_sin[SPECTRUM_HALF];
static u16 spectrum_bitrev[SPECTRUM_HALF];

/* Per-sample-rate band layout. */
static u32 spectrum_layout_rate;
static f32 spectrum_band_lo[SPECTRUM_BANDS];  /* fractional bin at the low edge  */
static f32 spectrum_band_hi[SPECTRUM_BANDS];
static f32 spectrum_band_tilt[SPECTRUM_BANDS];/* dB added to balance the slope */
static f32 spectrum_band_hz[SPECTRUM_BANDS];  /* center frequency              */

static void spectrum_init(void) {
    const f64 tau = 6.283185307179586;
    for (u32 i = 0; i < SPECTRUM_FFT; i++) {
        spectrum_window[i] = (f32)(0.5 - 0.5 * cos(tau * (f64)i / (f64)(SPECTRUM_FFT - 1)));
    }
    for (u32 k = 0; k < SPECTRUM_HALF; k++) {
        spectrum_cos[k] = (f32)cos(tau * (f64)k / (f64)SPECTRUM_FFT);
        spectrum_sin[k] = (f32)sin(tau * (f64)k / (f64)SPECTRUM_FFT);
    }
    u32 bits = 0;
    while ((1u << bits) < SPECTRUM_HALF) bits++;
    for (u32 i = 0; i < SPECTRUM_HALF; i++) {
        u32 r = 0;
        for (u32 b = 0; b < bits; b++) r |= ((i >> b) & 1u) << (bits - 1 - b);
        spectrum_bitrev[i] = (u16)r;
    }
    spectrum_ready = true;
}

static void spectrum_layout(u32 rate) {
    f32 bin_hz = (f32)rate / (f32)SPECTRUM_FFT;
    f32 ratio  = SPECTRUM_HI_HZ / SPECTRUM_LO_HZ;
    for (u32 b = 0; b < SPECTRUM_BANDS; b++) {
        f32 lo = SPECTRUM_LO_HZ * powf(ratio, (f32)b / (f32)SPECTRUM_BANDS);
        f32 hi = SPECTRUM_LO_HZ * powf(ratio, (f32)(b + 1) / (f32)SPECTRUM_BANDS);
        f32 nyquist = 0.5f * (f32)rate;
        if (hi > nyquist) hi = nyquist;
        if (lo > hi) lo = hi;
        f32 center = sqrtf(lo * hi);
        spectrum_band_lo[b]   = lo / bin_hz;
        spectrum_band_hi[b]   = hi / bin_hz;
        spectrum_band_hz[b]   = center;
        spectrum_band_tilt[b] = SPECTRUM_TILT_DB * log2f(center / 1000.0f);
    }
    spectrum_layout_rate = rate;
}

/* In-place iterative radix-2 complex FFT of size SPECTRUM_HALF. The N-point
   twiddles are reused with a stride of 2. */
static void spectrum_fft(f32 *re, f32 *im) {
    for (u32 i = 0; i < SPECTRUM_HALF; i++) {
        u32 j = spectrum_bitrev[i];
        if (j > i) {
            f32 t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (u32 size = 2; size <= SPECTRUM_HALF; size <<= 1) {
        u32 half   = size >> 1;
        u32 stride = (SPECTRUM_HALF / size) * 2; /* into the N-point tables */
        for (u32 start = 0; start < SPECTRUM_HALF; start += size) {
            for (u32 k = 0; k < half; k++) {
                f32 wr =  spectrum_cos[k * stride];
                f32 wi = -spectrum_sin[k * stride];
                u32 a = start + k, b = a + half;
                f32 xr = re[b] * wr - im[b] * wi;
                f32 xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
}

/* |X[k]|^2 for k < N/2 of the windowed real signal `x` (N samples). */
static void spectrum_power(const f32 *x, f32 *power) {
    f32 re[SPECTRUM_HALF], im[SPECTRUM_HALF];
    for (u32 n = 0; n < SPECTRUM_HALF; n++) {
        re[n] = x[2 * n]     * spectrum_window[2 * n];
        im[n] = x[2 * n + 1] * spectrum_window[2 * n + 1];
    }
    spectrum_fft(re, im);
    /* Split: X[k] = E[k] + W^k O[k], with E/O the spectra of the even/odd
       samples recovered from Z[k] and conj(Z[M-k]). */
    for (u32 k = 0; k < SPECTRUM_HALF; k++) {
        u32 m = (SPECTRUM_HALF - k) & (SPECTRUM_HALF - 1);
        f32 ar = re[k], ai = im[k], br = re[m], bi = im[m];
        f32 er = 0.5f * (ar + br), ei = 0.5f * (ai - bi);
        f32 or_ = 0.5f * (ai + bi), oi = -0.5f * (ar - br);
        f32 c = spectrum_cos[k], s = spectrum_sin[k];
        f32 xr = er + c * or_ + s * oi;
        f32 xi = ei + c * oi - s * or_;
        power[k] = xr * xr + xi * xi;
    }
}

static f32 spectrum_smooth(f32 cur, f32 target, f32 attack, f32 release) {
    return cur + (target - cur) * (target > cur ? attack : release);
}

void spectrum_update(Spectrum *s, const f32 *samples, u32 count, u32 sample_rate, f32 dt) {
    if (!s) return;
    if (!spectrum_ready) spectrum_init();
    if (!(dt > 0)) dt = 0;
    if (dt > 0.25f) dt = 0.25f;
    if (sample_rate == 0) sample_rate = s->sample_rate ? s->sample_rate : 48000;
    if (sample_rate != spectrum_layout_rate) spectrum_layout(sample_rate);
    s->sample_rate = sample_rate;

    f32 target[SPECTRUM_BANDS] = {0};
    f32 t_bass = 0, t_mid = 0, t_treble = 0, t_level = 0;

    if (samples && count > 0) {
        /* The latest SPECTRUM_FFT samples, zero-padded in front if short. */
        f32 x[SPECTRUM_FFT];
        u32 n = count < SPECTRUM_FFT ? count : SPECTRUM_FFT;
        memset(x, 0, sizeof(f32) * (SPECTRUM_FFT - n));
        memcpy(x + (SPECTRUM_FFT - n), samples + (count - n), sizeof(f32) * n);

        f64 sum_sq = 0;
        for (u32 i = SPECTRUM_FFT - n; i < SPECTRUM_FFT; i++) sum_sq += (f64)x[i] * (f64)x[i];
        f32 rms = (f32)sqrt(sum_sq / (f64)n);
        t_level = (20.0f * log10f(rms + 1e-9f) + 50.0f) / 50.0f;

        f32 power[SPECTRUM_HALF];
        spectrum_power(x, power);

        /* Hann: a full-scale sine centered on a bin has |X| = N/4. */
        const f32 norm = (4.0f / (f32)SPECTRUM_FFT) * (4.0f / (f32)SPECTRUM_FFT);
        u32 n_bass = 0, n_mid = 0, n_treble = 0;
        for (u32 b = 0; b < SPECTRUM_BANDS; b++) {
            f32 lo = spectrum_band_lo[b], hi = spectrum_band_hi[b];
            u32 first = (u32)ceilf(lo), last = (u32)floorf(hi);
            f32 p;
            if (last >= SPECTRUM_HALF) last = SPECTRUM_HALF - 1;
            if (first > last) {
                /* Narrower than a bin: interpolate at the band center. */
                f32 c  = 0.5f * (lo + hi);
                u32 i0 = (u32)c;
                if (i0 >= SPECTRUM_HALF - 1) i0 = SPECTRUM_HALF - 2;
                f32 t  = c - (f32)i0;
                f32 m  = (1 - t) * sqrtf(power[i0]) + t * sqrtf(power[i0 + 1]);
                p = m * m;
            } else {
                p = 0;
                for (u32 i = first; i <= last; i++) if (power[i] > p) p = power[i];
            }
            f32 db = 10.0f * log10f(p * norm + 1e-12f) + spectrum_band_tilt[b];
            f32 v  = (db - SPECTRUM_FLOOR_DB) / -SPECTRUM_FLOOR_DB;
            v = v < 0 ? 0 : v > 1 ? 1 : v;
            target[b] = v;

            f32 hz = spectrum_band_hz[b];
            if      (hz < 150.0f)  { t_bass   += v; n_bass++; }
            else if (hz < 2000.0f) { t_mid    += v; n_mid++; }
            else                   { t_treble += v; n_treble++; }
        }
        if (n_bass)   t_bass   /= (f32)n_bass;
        if (n_mid)    t_mid    /= (f32)n_mid;
        if (n_treble) t_treble /= (f32)n_treble;
        t_level = t_level < 0 ? 0 : t_level > 1 ? 1 : t_level;
    }

    f32 attack  = 1.0f - expf(-dt * 30.0f);
    f32 release = 1.0f - expf(-dt * 8.0f);
    f32 fall    = dt * 0.5f; /* peaks: units per second */
    for (u32 b = 0; b < SPECTRUM_BANDS; b++) {
        s->bands[b] = spectrum_smooth(s->bands[b], target[b], attack, release);
        s->peaks[b] = s->bands[b] > s->peaks[b] - fall ? s->bands[b] : s->peaks[b] - fall;
        if (s->peaks[b] < 0) s->peaks[b] = 0;
    }
    s->bass   = spectrum_smooth(s->bass,   t_bass,   attack, release);
    s->mid    = spectrum_smooth(s->mid,    t_mid,    attack, release);
    s->treble = spectrum_smooth(s->treble, t_treble, attack, release);
    s->level  = spectrum_smooth(s->level,  t_level,  attack, release);

    /* Onsets: raw bass jumping clearly above its running average. The
       refractory period (~250 ms) falls out of the decay: after 250 ms the
       beat value is exp(-6 * 0.25) ~= 0.22. */
    s->beat *= expf(-dt * 6.0f);
    if (count > 0 && t_bass > 0.25f && t_bass > s->bass_avg + 0.08f && s->beat < 0.22f) {
        s->beat = 1.0f;
    }
    s->bass_avg += (t_bass - s->bass_avg) * (1.0f - expf(-dt * 3.0f));
}
