#include "core/math.h"
#include "core/memory.c" /* third-party objects allocate through the tracked heap */

#include <stdio.h>

#define TEST_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "math test failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
        return 1; \
    } \
} while (0)

static b32 near(f32 a, f32 b) { return core_f32_near(a, b, 0.0001f); }
static b32 vec2_near(vec2 a, vec2 b) { return near(a.x, b.x) && near(a.y, b.y); }
static b32 vec3_near(vec3 a, vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }
static b32 vec4_near(vec4 a, vec4 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z) && near(a.w, b.w); }

int main(void)
{
    _Static_assert(sizeof(vec2) == 8, "vec2 must stay compact");
    _Static_assert(sizeof(vec3) == 12, "vec3 must stay compact");
    _Static_assert(sizeof(vec4) == 16, "vec4 must stay compact");
    _Static_assert(sizeof(mat4) == 64, "mat4 must stay compact");

    TEST_CHECK(vec2_near(vec2_add(vec2_make(1, 2), vec2_make(3, 4)), vec2_make(4, 6)));
    TEST_CHECK(near(vec3_dot(vec3_make(1, 2, 3), vec3_make(4, 5, 6)), 32));
    TEST_CHECK(vec3_near(vec3_cross(vec3_make(1, 0, 0), vec3_make(0, 1, 0)), vec3_make(0, 0, 1)));
    TEST_CHECK(vec3_near(vec3_normalize_safe(vec3_make(0, 0, 0)), vec3_zero()));
    TEST_CHECK(near(vec3_length(vec3_make(3, 4, 0)), 5));
    TEST_CHECK(near(core_wrap(-0.25f, 1.0f), 0.75f));
    TEST_CHECK(near(core_radians(180.0f), CORE_PI));

    vec3 rotated = quat_rotate_vec3(quat_from_axis_angle(vec3_make(0, 0, 1), CORE_PI * 0.5f), vec3_make(1, 0, 0));
    TEST_CHECK(vec3_near(rotated, vec3_make(0, 1, 0)));

    mat4 model = mat4_mul(mat4_translate(vec3_make(1, 2, 3)), mat4_scale(vec3_make(2, 2, 2)));
    TEST_CHECK(vec3_near(mat4_transform_point(model, vec3_make(1, 1, 1)), vec3_make(3, 4, 5)));
    TEST_CHECK(vec3_near(mat4_transform_vector(mat4_translate(vec3_make(10, 20, 30)), vec3_make(1, 2, 3)), vec3_make(1, 2, 3)));
    TEST_CHECK(vec3_near(mat4_transform_point(mat4_rotate_z(CORE_PI * 0.5f), vec3_make(1, 0, 0)), vec3_make(0, 1, 0)));

    mat4 ortho = mat4_orthographic(-1, 1, -1, 1, -1, 1);
    TEST_CHECK(vec3_near(mat4_transform_point(ortho, vec3_make(1, 1, 1)), vec3_make(1, 1, -1)));
    TEST_CHECK(vec3_near(mat4_transform_point(mat4_look_at(vec3_zero(), vec3_make(0, 0, -1), vec3_make(0, 1, 0)), vec3_make(0, 0, -1)), vec3_make(0, 0, -1)));

    rect2 r = { .min = vec2_make(0, 0), .max = vec2_make(10, 10) };
    TEST_CHECK(rect2_contains(r, vec2_make(5, 5)));
    TEST_CHECK(!rect2_contains(r, vec2_make(11, 5)));
    TEST_CHECK(vec2_near(rect2_clamp_point(r, vec2_make(-2, 12)), vec2_make(0, 10)));

    aabb3 box = { .min = vec3_make(0, 0, 0), .max = vec3_make(1, 1, 1) };
    TEST_CHECK(aabb3_contains(box, vec3_make(0.5f, 0.5f, 0.5f)));
    TEST_CHECK(aabb3_intersects(box, (aabb3){ .min = vec3_make(1, 1, 1), .max = vec3_make(2, 2, 2) }));
    TEST_CHECK(vec3_near(aabb3_expand(box, vec3_make(-1, 2, 3)).min, vec3_make(-1, 0, 0)));

    vec3 red = core_hsv_to_rgb(0.0f, 1.0f, 1.0f);
    TEST_CHECK(vec3_near(red, vec3_make(1, 0, 0)));
    TEST_CHECK(vec4_near(core_hex(0x3366CCFF), vec4_make(0x33 / 255.0f, 0x66 / 255.0f, 0xCC / 255.0f, 1.0f)));

    puts("math tests passed");
    return 0;
}

