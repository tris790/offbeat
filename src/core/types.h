#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef float    f32;
typedef double   f64;

typedef size_t   usize;
typedef ptrdiff_t isize;

typedef s32      b32;

#define CORE_KB(n) ((u64)(n) << 10)
#define CORE_MB(n) ((u64)(n) << 20)
#define CORE_GB(n) ((u64)(n) << 30)

#define CORE_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CORE_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CORE_CLAMP(x, lo, hi) CORE_MIN(CORE_MAX(x, lo), hi)
#define CORE_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define CORE_ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define CORE_ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

#if defined(__GNUC__) || defined(__clang__)
    #define CORE_INLINE static inline __attribute__((always_inline))
#else
    #define CORE_INLINE static inline
#endif

#define CORE_UNUSED(x) ((void)(x))

#endif /* CORE_TYPES_H */
