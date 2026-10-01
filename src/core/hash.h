#ifndef CORE_HASH_H
#define CORE_HASH_H

#include "types.h"

/* Stable FNV-1a identity hashes, shared by paths and UI names. Non-cryptographic;
   tables storing arbitrary strings must still compare bytes on collisions. */
CORE_INLINE u64 core_hash_bytes(const void *data, u64 len) {
    const u8 *p = data;
    u64 h = 0xcbf29ce484222325ULL;
    for (u64 i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}
CORE_INLINE u64 core_hash_cstr(const char *s) {
    u64 h = 0xcbf29ce484222325ULL;
    if (s) for (const u8 *p = (const u8 *)s; *p; p++) { h ^= *p; h *= 0x100000001b3ULL; }
    return h;
}

/* MurmurHash3 finalizer used for glyph/kerning table distribution. */
CORE_INLINE u64 core_hash_u64(u64 x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* SplitMix64 finalizer used in persistent cover keys. Kept distinct so moving
   implementations does not invalidate existing caches or alter palettes. */
CORE_INLINE u64 core_mix_u64(u64 x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

#endif
