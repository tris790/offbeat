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
    TEST_CHECK(near(core_saturate(-1), 0) && near(core_saturate(2), 1));
    TEST_CHECK(near(core_saturate(0.4f), 0.4f));
    TEST_CHECK(isnan(core_saturate(NAN)));
    TEST_CHECK(near(core_ease_out_cubic(-0.5f), 0));
    TEST_CHECK(near(core_ease_out_cubic(0.5f), 0.875f));
    TEST_CHECK(near(core_ease_out_cubic(1.5f), 1));

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
    TEST_CHECK(vec3_near(core_hsv_to_rgb(-0.25f, 0.7f, 0.8f), core_hsv_to_rgb(0.75f, 0.7f, 0.8f)));
    TEST_CHECK(vec3_near(core_hsv_to_rgb(2.25f, 0.7f, 0.8f), core_hsv_to_rgb(0.25f, 0.7f, 0.8f)));
    TEST_CHECK(vec3_near(core_rgb_to_hsv(vec3_zero()), vec3_zero()));
    TEST_CHECK(vec3_near(core_rgb_to_hsv(vec3_make(0.4f, 0.4f, 0.4f)), vec3_make(0, 0, 0.4f)));
    TEST_CHECK(vec3_near(core_rgb_to_hsv(vec3_make(1, 0, 1)), vec3_make(5.0f / 6.0f, 1, 1)));
    /* Cover palettes use all hue branches and non-primary RGB components. */
    const vec3 colors[] = {
        {{1, 0, 0}}, {{0, 1, 0}}, {{0, 0, 1}}, {{0.2f, 0.7f, 0.5f}},
        {{0.8f, 0.1f, 0.6f}}, {{0.3f, 0.2f, 0.9f}},
    };
    for (u32 i = 0; i < CORE_ARRAY_COUNT(colors); i++) {
        vec3 hsv = core_rgb_to_hsv(colors[i]);
        TEST_CHECK(vec3_near(core_hsv_to_rgb(hsv.x, hsv.y, hsv.z), colors[i]));
    }
    TEST_CHECK(near(core_rgb_luma(vec3_one()), 1));
    TEST_CHECK(near(core_rgb_luma(vec3_make(0, 1, 0)), 0.7152f));
    /* An asymmetric matrix catches row/column-order swaps; extremes exercise
       the clamping shared by CPU scene colors and renderer instance colors. */
    const f32 color_matrix[9] = { 0, 2, 0, 0, 0, 0.5f, -1, 0, 0 };
    TEST_CHECK(vec3_near(core_color_matrix_apply(color_matrix, vec3_make(0.2f, 0.7f, 0.8f)),
                        vec3_make(1, 0.4f, 0)));
    TEST_CHECK(vec4_near(core_hex(0x3366CCFF), vec4_make(0x33 / 255.0f, 0x66 / 255.0f, 0xCC / 255.0f, 1.0f)));

    puts("math tests passed");
    return 0;
}
