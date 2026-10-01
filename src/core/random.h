#ifndef CORE_RANDOM_H
#define CORE_RANDOM_H

#include "types.h"

/* Xorshift32 for shuffle, jitter and particles. Explicit caller-owned state;
   nonzero seeds preserve the app's existing sequences. Not cryptographic. */
CORE_INLINE u32 core_rng_next(u32 *state) {
    u32 x = *state ? *state : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}

/* Uniform 24-bit value in [0, 1). */
CORE_INLINE f32 core_rng_f32(u32 *state) {
    return (f32)(core_rng_next(state) & 0xFFFFFFu) / (f32)0x1000000;
}

#endif
