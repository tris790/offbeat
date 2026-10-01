#ifndef CORE_MATH_H
#define CORE_MATH_H

#include "types.h"

#include <math.h>

/*
 * Small, allocation-free game math primitives.
 *
 * Matrices are column-major and multiply column vectors, matching OpenGL.
 * Angles are radians. All operations use f32 deliberately: this keeps the
 * data compact and is sufficient for gameplay, rendering, and camera work.
 *
 * The functions are inline so TinyCC can eliminate call overhead in hot code.
 * No SIMD or compiler-specific vector extensions are required.
 */

#define CORE_PI       3.14159265358979323846f
#define CORE_TAU      6.28318530717958647692f
#define CORE_EPSILON  0.000001f

typedef union {
    struct { f32 x, y; };
    f32 e[2];
} vec2;

typedef union {
    struct { f32 x, y, z; };
    struct { f32 r, g, b; };
    f32 e[3];
} vec3;

typedef union {
    struct { f32 x, y, z, w; };
    struct { f32 r, g, b, a; };
    f32 e[4];
} vec4;

typedef struct { f32 e[16]; } mat4;
typedef struct { f32 x, y, z, w; } quat;

typedef struct { vec2 min, max; } rect2;
typedef struct { vec3 min, max; } aabb3;
typedef struct {
    vec3 position;
    quat rotation;
    vec3 scale;
} transform3;

CORE_INLINE f32 core_f32_min(f32 a, f32 b) { return a < b ? a : b; }
CORE_INLINE f32 core_f32_max(f32 a, f32 b) { return a > b ? a : b; }
CORE_INLINE f32 core_f32_clamp(f32 x, f32 lo, f32 hi) {
    return core_f32_min(core_f32_max(x, lo), hi);
}
/* Clamp a normalized value; NaN propagates like the original UI helpers. */
CORE_INLINE f32 core_saturate(f32 x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
CORE_INLINE f32 core_ease_out_cubic(f32 t) {
    f32 u = 1.0f - core_saturate(t);
    return 1.0f - u * u * u;
}
CORE_INLINE f32 core_f32_abs(f32 x) { return x < 0.0f ? -x : x; }
CORE_INLINE b32 core_f32_near(f32 a, f32 b, f32 epsilon) {
    return core_f32_abs(a - b) <= epsilon;
}
CORE_INLINE f32 core_lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
CORE_INLINE f32 core_inverse_lerp(f32 a, f32 b, f32 x) {
    f32 d = b - a;
    return core_f32_abs(d) > CORE_EPSILON ? (x - a) / d : 0.0f;
}
CORE_INLINE f32 core_smoothstep(f32 edge0, f32 edge1, f32 x) {
    f32 t = core_f32_clamp(core_inverse_lerp(edge0, edge1, x), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
CORE_INLINE f32 core_move_toward(f32 current, f32 target, f32 max_delta) {
    f32 d = target - current;
    if (core_f32_abs(d) <= max_delta) return target;
    return current + (d < 0.0f ? -max_delta : max_delta);
}
CORE_INLINE f32 core_wrap(f32 x, f32 period) {
    if (period <= 0.0f) return 0.0f;
    x = fmodf(x, period);
    return x < 0.0f ? x + period : x;
}
CORE_INLINE f32 core_radians(f32 degrees) { return degrees * (CORE_PI / 180.0f); }
CORE_INLINE f32 core_degrees(f32 radians) { return radians * (180.0f / CORE_PI); }

CORE_INLINE vec2 vec2_make(f32 x, f32 y) { return (vec2){ .x = x, .y = y }; }
CORE_INLINE vec2 vec2_zero(void) { return (vec2){0}; }
CORE_INLINE vec2 vec2_one(void) { return vec2_make(1.0f, 1.0f); }
CORE_INLINE vec2 vec2_add(vec2 a, vec2 b) { return vec2_make(a.x + b.x, a.y + b.y); }
CORE_INLINE vec2 vec2_sub(vec2 a, vec2 b) { return vec2_make(a.x - b.x, a.y - b.y); }
CORE_INLINE vec2 vec2_neg(vec2 a) { return vec2_make(-a.x, -a.y); }
CORE_INLINE vec2 vec2_mul(vec2 a, f32 s) { return vec2_make(a.x * s, a.y * s); }
CORE_INLINE vec2 vec2_div(vec2 a, f32 s) { return vec2_mul(a, 1.0f / s); }
CORE_INLINE vec2 vec2_hadamard(vec2 a, vec2 b) { return vec2_make(a.x * b.x, a.y * b.y); }
CORE_INLINE f32 vec2_dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
CORE_INLINE f32 vec2_length_sq(vec2 a) { return vec2_dot(a, a); }
CORE_INLINE f32 vec2_length(vec2 a) { return sqrtf(vec2_length_sq(a)); }
CORE_INLINE f32 vec2_distance_sq(vec2 a, vec2 b) { return vec2_length_sq(vec2_sub(a, b)); }
CORE_INLINE f32 vec2_distance(vec2 a, vec2 b) { return sqrtf(vec2_distance_sq(a, b)); }
CORE_INLINE vec2 vec2_normalize_safe(vec2 a) {
    f32 len_sq = vec2_length_sq(a);
    return len_sq > CORE_EPSILON * CORE_EPSILON ? vec2_mul(a, 1.0f / sqrtf(len_sq)) : vec2_zero();
}
CORE_INLINE vec2 vec2_lerp(vec2 a, vec2 b, f32 t) { return vec2_make(core_lerp(a.x, b.x, t), core_lerp(a.y, b.y, t)); }
CORE_INLINE vec2 vec2_perpendicular(vec2 a) { return vec2_make(-a.y, a.x); }
CORE_INLINE vec2 vec2_rotate(vec2 a, f32 radians) {
    f32 c = cosf(radians), s = sinf(radians);
    return vec2_make(a.x * c - a.y * s, a.x * s + a.y * c);
}
CORE_INLINE vec2 vec2_reflect(vec2 v, vec2 normal) {
    return vec2_sub(v, vec2_mul(normal, 2.0f * vec2_dot(v, normal)));
}

CORE_INLINE vec3 vec3_make(f32 x, f32 y, f32 z) { return (vec3){ .x = x, .y = y, .z = z }; }
CORE_INLINE vec3 vec3_zero(void) { return (vec3){0}; }
CORE_INLINE vec3 vec3_one(void) { return (vec3){ .x = 1.0f, .y = 1.0f, .z = 1.0f }; }
CORE_INLINE vec3 vec3_add(vec3 a, vec3 b) { return vec3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
CORE_INLINE vec3 vec3_sub(vec3 a, vec3 b) { return vec3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
CORE_INLINE vec3 vec3_neg(vec3 a) { return vec3_make(-a.x, -a.y, -a.z); }
CORE_INLINE vec3 vec3_mul(vec3 a, f32 s) { return vec3_make(a.x * s, a.y * s, a.z * s); }
CORE_INLINE vec3 vec3_div(vec3 a, f32 s) { return vec3_mul(a, 1.0f / s); }
CORE_INLINE vec3 vec3_hadamard(vec3 a, vec3 b) { return vec3_make(a.x * b.x, a.y * b.y, a.z * b.z); }
CORE_INLINE f32 vec3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
CORE_INLINE vec3 vec3_cross(vec3 a, vec3 b) {
    return vec3_make(a.y * b.z - a.z * b.y,
                     a.z * b.x - a.x * b.z,
                     a.x * b.y - a.y * b.x);
}
CORE_INLINE f32 vec3_length_sq(vec3 a) { return vec3_dot(a, a); }
CORE_INLINE f32 vec3_length(vec3 a) { return sqrtf(vec3_length_sq(a)); }
CORE_INLINE f32 vec3_distance_sq(vec3 a, vec3 b) { return vec3_length_sq(vec3_sub(a, b)); }
CORE_INLINE f32 vec3_distance(vec3 a, vec3 b) { return sqrtf(vec3_distance_sq(a, b)); }
CORE_INLINE vec3 vec3_normalize_safe(vec3 a) {
    f32 len_sq = vec3_length_sq(a);
    return len_sq > CORE_EPSILON * CORE_EPSILON ? vec3_mul(a, 1.0f / sqrtf(len_sq)) : vec3_zero();
}
CORE_INLINE vec3 vec3_lerp(vec3 a, vec3 b, f32 t) {
    return vec3_make(core_lerp(a.x, b.x, t), core_lerp(a.y, b.y, t), core_lerp(a.z, b.z, t));
}
CORE_INLINE vec3 vec3_reflect(vec3 v, vec3 normal) {
    return vec3_sub(v, vec3_mul(normal, 2.0f * vec3_dot(v, normal)));
}
CORE_INLINE vec3 vec3_project(vec3 a, vec3 onto) {
    f32 d = vec3_length_sq(onto);
    return d > CORE_EPSILON * CORE_EPSILON ? vec3_mul(onto, vec3_dot(a, onto) / d) : vec3_zero();
}

CORE_INLINE vec4 vec4_make(f32 x, f32 y, f32 z, f32 w) { return (vec4){ .x = x, .y = y, .z = z, .w = w }; }
CORE_INLINE vec4 vec4_zero(void) { return (vec4){0}; }
CORE_INLINE vec4 vec4_one(void) { return (vec4){ .x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f }; }
CORE_INLINE vec4 vec4_add(vec4 a, vec4 b) { return vec4_make(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
CORE_INLINE vec4 vec4_sub(vec4 a, vec4 b) { return vec4_make(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
CORE_INLINE vec4 vec4_mul(vec4 a, f32 s) { return vec4_make(a.x * s, a.y * s, a.z * s, a.w * s); }
CORE_INLINE vec4 vec4_hadamard(vec4 a, vec4 b) { return vec4_make(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w); }
CORE_INLINE f32 vec4_dot(vec4 a, vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
CORE_INLINE f32 vec4_length_sq(vec4 a) { return vec4_dot(a, a); }
CORE_INLINE vec4 vec4_normalize_safe(vec4 a) {
    f32 len_sq = vec4_length_sq(a);
    return len_sq > CORE_EPSILON * CORE_EPSILON ? vec4_mul(a, 1.0f / sqrtf(len_sq)) : vec4_zero();
}
CORE_INLINE vec4 vec4_lerp(vec4 a, vec4 b, f32 t) { return vec4_add(a, vec4_mul(vec4_sub(b, a), t)); }

CORE_INLINE quat quat_make(f32 x, f32 y, f32 z, f32 w) { return (quat){x, y, z, w}; }
CORE_INLINE quat quat_identity(void) { return quat_make(0.0f, 0.0f, 0.0f, 1.0f); }
CORE_INLINE quat quat_conjugate(quat q) { return quat_make(-q.x, -q.y, -q.z, q.w); }
CORE_INLINE f32 quat_length_sq(quat q) { return q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w; }
CORE_INLINE quat quat_normalize_safe(quat q) {
    f32 len_sq = quat_length_sq(q);
    if (len_sq <= CORE_EPSILON * CORE_EPSILON) return quat_identity();
    f32 inv_len = 1.0f / sqrtf(len_sq);
    return quat_make(q.x * inv_len, q.y * inv_len, q.z * inv_len, q.w * inv_len);
}
CORE_INLINE quat quat_mul(quat a, quat b) {
    return quat_make(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                     a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                     a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                     a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
CORE_INLINE quat quat_from_axis_angle(vec3 axis, f32 radians) {
    vec3 n = vec3_normalize_safe(axis);
    f32 half = radians * 0.5f, s = sinf(half);
    if (vec3_length_sq(n) <= CORE_EPSILON * CORE_EPSILON) return quat_identity();
    return quat_make(n.x * s, n.y * s, n.z * s, cosf(half));
}
CORE_INLINE vec3 quat_rotate_vec3(quat q, vec3 v) {
    vec3 qv = vec3_make(q.x, q.y, q.z);
    vec3 t = vec3_mul(vec3_cross(qv, v), 2.0f);
    return vec3_add(v, vec3_add(vec3_mul(t, q.w), vec3_cross(qv, t)));
}
CORE_INLINE quat quat_nlerp(quat a, quat b, f32 t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f)
        b = quat_make(-b.x, -b.y, -b.z, -b.w);
    return quat_normalize_safe(quat_make(core_lerp(a.x, b.x, t), core_lerp(a.y, b.y, t), core_lerp(a.z, b.z, t), core_lerp(a.w, b.w, t)));
}

CORE_INLINE mat4 mat4_zero(void) { return (mat4){0}; }
CORE_INLINE mat4 mat4_identity(void) {
    mat4 m = {0};
    m.e[0] = m.e[5] = m.e[10] = m.e[15] = 1.0f;
    return m;
}
CORE_INLINE mat4 mat4_mul(mat4 a, mat4 b) {
    mat4 out;
    for (u32 col = 0; col < 4; col++)
        for (u32 row = 0; row < 4; row++)
            out.e[col * 4 + row] = a.e[row] * b.e[col * 4] + a.e[4 + row] * b.e[col * 4 + 1] + a.e[8 + row] * b.e[col * 4 + 2] + a.e[12 + row] * b.e[col * 4 + 3];
    return out;
}
CORE_INLINE mat4 mat4_transpose(mat4 a) {
    mat4 out;
    for (u32 col = 0; col < 4; col++)
        for (u32 row = 0; row < 4; row++) out.e[col * 4 + row] = a.e[row * 4 + col];
    return out;
}
CORE_INLINE mat4 mat4_translate(vec3 t) { mat4 m = mat4_identity(); m.e[12] = t.x; m.e[13] = t.y; m.e[14] = t.z; return m; }
CORE_INLINE mat4 mat4_scale(vec3 s) { mat4 m = mat4_identity(); m.e[0] = s.x; m.e[5] = s.y; m.e[10] = s.z; return m; }
CORE_INLINE mat4 mat4_rotate_x(f32 r) { f32 c = cosf(r), s = sinf(r); mat4 m = mat4_identity(); m.e[5] = c; m.e[9] = -s; m.e[6] = s; m.e[10] = c; return m; }
CORE_INLINE mat4 mat4_rotate_y(f32 r) { f32 c = cosf(r), s = sinf(r); mat4 m = mat4_identity(); m.e[0] = c; m.e[8] = s; m.e[2] = -s; m.e[10] = c; return m; }
CORE_INLINE mat4 mat4_rotate_z(f32 r) { f32 c = cosf(r), s = sinf(r); mat4 m = mat4_identity(); m.e[0] = c; m.e[4] = -s; m.e[1] = s; m.e[5] = c; return m; }
CORE_INLINE mat4 mat4_from_quat(quat q) {
    q = quat_normalize_safe(q);
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    mat4 m = mat4_identity();
    m.e[0] = 1.0f - 2.0f * (yy + zz); m.e[4] = 2.0f * (xy - wz);       m.e[8]  = 2.0f * (xz + wy);
    m.e[1] = 2.0f * (xy + wz);       m.e[5] = 1.0f - 2.0f * (xx + zz); m.e[9]  = 2.0f * (yz - wx);
    m.e[2] = 2.0f * (xz - wy);       m.e[6] = 2.0f * (yz + wx);       m.e[10] = 1.0f - 2.0f * (xx + yy);
    return m;
}
CORE_INLINE mat4 mat4_perspective(f32 fov_y, f32 aspect, f32 near_z, f32 far_z) {
    mat4 m = mat4_zero();
    f32 f = 1.0f / tanf(fov_y * 0.5f);
    m.e[0] = f / aspect; m.e[5] = f;
    m.e[10] = (far_z + near_z) / (near_z - far_z);
    m.e[11] = -1.0f;
    m.e[14] = (2.0f * far_z * near_z) / (near_z - far_z);
    return m;
}
CORE_INLINE mat4 mat4_orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 near_z, f32 far_z) {
    mat4 m = mat4_identity();
    m.e[0] = 2.0f / (right - left); m.e[5] = 2.0f / (top - bottom); m.e[10] = -2.0f / (far_z - near_z);
    m.e[12] = -(right + left) / (right - left); m.e[13] = -(top + bottom) / (top - bottom); m.e[14] = -(far_z + near_z) / (far_z - near_z);
    return m;
}
CORE_INLINE mat4 mat4_look_at(vec3 eye, vec3 target, vec3 up) {
    vec3 f = vec3_normalize_safe(vec3_sub(target, eye));
    vec3 s = vec3_normalize_safe(vec3_cross(f, up));
    vec3 u = vec3_cross(s, f);
    mat4 m = mat4_identity();
    m.e[0] = s.x; m.e[1] = u.x; m.e[2] = -f.x;
    m.e[4] = s.y; m.e[5] = u.y; m.e[6] = -f.y;
    m.e[8] = s.z; m.e[9] = u.z; m.e[10] = -f.z;
    m.e[12] = -vec3_dot(s, eye); m.e[13] = -vec3_dot(u, eye); m.e[14] = vec3_dot(f, eye);
    return m;
}
CORE_INLINE vec4 mat4_transform_vec4(mat4 m, vec4 v) {
    return vec4_make(m.e[0] * v.x + m.e[4] * v.y + m.e[8] * v.z + m.e[12] * v.w,
                     m.e[1] * v.x + m.e[5] * v.y + m.e[9] * v.z + m.e[13] * v.w,
                     m.e[2] * v.x + m.e[6] * v.y + m.e[10] * v.z + m.e[14] * v.w,
                     m.e[3] * v.x + m.e[7] * v.y + m.e[11] * v.z + m.e[15] * v.w);
}
CORE_INLINE vec3 mat4_transform_vector(mat4 m, vec3 v) { vec4 r = mat4_transform_vec4(m, vec4_make(v.x, v.y, v.z, 0.0f)); return vec3_make(r.x, r.y, r.z); }
CORE_INLINE vec3 mat4_transform_point(mat4 m, vec3 p) {
    vec4 r = mat4_transform_vec4(m, vec4_make(p.x, p.y, p.z, 1.0f));
    return core_f32_abs(r.w) > CORE_EPSILON && r.w != 1.0f ? vec3_make(r.x / r.w, r.y / r.w, r.z / r.w) : vec3_make(r.x, r.y, r.z);
}

CORE_INLINE transform3 transform3_identity(void) {
    return (transform3){ .position = vec3_zero(), .rotation = quat_identity(), .scale = vec3_one() };
}
CORE_INLINE mat4 transform3_matrix(transform3 t) {
    return mat4_mul(mat4_translate(t.position), mat4_mul(mat4_from_quat(t.rotation), mat4_scale(t.scale)));
}

CORE_INLINE b32 rect2_contains(rect2 r, vec2 p) { return p.x >= r.min.x && p.y >= r.min.y && p.x <= r.max.x && p.y <= r.max.y; }
CORE_INLINE vec2 rect2_clamp_point(rect2 r, vec2 p) { return vec2_make(core_f32_clamp(p.x, r.min.x, r.max.x), core_f32_clamp(p.y, r.min.y, r.max.y)); }
CORE_INLINE b32 rect2_intersects(rect2 a, rect2 b) { return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y; }
CORE_INLINE b32 aabb3_contains(aabb3 b, vec3 p) { return p.x >= b.min.x && p.y >= b.min.y && p.z >= b.min.z && p.x <= b.max.x && p.y <= b.max.y && p.z <= b.max.z; }
CORE_INLINE b32 aabb3_intersects(aabb3 a, aabb3 b) { return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z; }
CORE_INLINE aabb3 aabb3_expand(aabb3 b, vec3 p) {
    b.min.x = core_f32_min(b.min.x, p.x); b.min.y = core_f32_min(b.min.y, p.y); b.min.z = core_f32_min(b.min.z, p.z);
    b.max.x = core_f32_max(b.max.x, p.x); b.max.y = core_f32_max(b.max.y, p.y); b.max.z = core_f32_max(b.max.z, p.z);
    return b;
}

CORE_INLINE vec4 core_rgba(f32 r, f32 g, f32 b, f32 a) { return vec4_make(r, g, b, a); }
CORE_INLINE vec4 core_hex(u32 hex) {
    return vec4_make(((hex >> 24) & 0xFF) / 255.0f, ((hex >> 16) & 0xFF) / 255.0f,
                     ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f);
}
/* Hue in turns, saturation/value in 0..1. Achromatic colors have hue 0. */
CORE_INLINE vec3 core_rgb_to_hsv(vec3 c) {
    f32 mx = CORE_MAX(c.r, CORE_MAX(c.g, c.b));
    f32 mn = CORE_MIN(c.r, CORE_MIN(c.g, c.b)), d = mx - mn;
    f32 s = mx > 0 ? d / mx : 0;
    if (d <= 0) return vec3_make(0, s, mx);
    f32 h;
    if (mx == c.r)      h = (c.g - c.b) / d;
    else if (mx == c.g) h = 2 + (c.b - c.r) / d;
    else               h = 4 + (c.r - c.g) / d;
    h /= 6;
    return vec3_make(h < 0 ? h + 1 : h, s, mx);
}
/* Rec.709 weighted RGB brightness. Physical luminance requires linear RGB;
   this helper does not convert the supplied color's transfer function. */
CORE_INLINE f32 core_rgb_luma(vec3 c) {
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}
/* Row-major color matrix (unlike mat4), with output clamped to display RGB. */
CORE_INLINE vec3 core_color_matrix_apply(const f32 m[9], vec3 c) {
    vec3 o = vec3_make(m[0] * c.r + m[1] * c.g + m[2] * c.b,
                      m[3] * c.r + m[4] * c.g + m[5] * c.b,
                      m[6] * c.r + m[7] * c.g + m[8] * c.b);
    o.r = CORE_CLAMP(o.r, 0.0f, 1.0f);
    o.g = CORE_CLAMP(o.g, 0.0f, 1.0f);
    o.b = CORE_CLAMP(o.b, 0.0f, 1.0f);
    return o;
}
CORE_INLINE vec3 core_hsv_to_rgb(f32 h, f32 s, f32 v) {
    h -= floorf(h);
    f32 scaled = h * 6.0f, i = floorf(scaled), f = scaled - i;
    f32 p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
    switch ((int)i % 6) {
    case 0: return vec3_make(v, t, p);
    case 1: return vec3_make(q, v, p);
    case 2: return vec3_make(p, v, t);
    case 3: return vec3_make(p, q, v);
    case 4: return vec3_make(t, p, v);
    default: return vec3_make(v, p, q);
    }
}

#endif /* CORE_MATH_H */
