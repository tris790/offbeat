#include "image.h"

#include <math.h>
#include <string.h>

void core_image_color_tables_init(Core_ImageColorTables *tables) {
    for (u32 i = 0; i < 256; i++) {
        f32 v = i / 255.0f;
        tables->srgb_to_linear[i] = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    }
    for (u32 i = 0; i < 4096; i++) {
        f32 v = i / 4095.0f;
        f32 s = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
        tables->linear_to_srgb[i] = (u8)CORE_CLAMP((s32)(s * 255.0f + 0.5f), 0, 255);
    }
}

static u8 core_image_to_srgb(const Core_ImageColorTables *tables, f32 lin) {
    s32 i = (s32)(lin * 4095.0f + 0.5f);
    return tables->linear_to_srgb[CORE_CLAMP(i, 0, 4095)];
}

static void core_image_fetch(const Core_ImageColorTables *tables, const u8 *p, u32 ch, f32 *o) {
    if (ch >= 3) {
        o[0] = tables->srgb_to_linear[p[0]]; o[1] = tables->srgb_to_linear[p[1]]; o[2] = tables->srgb_to_linear[p[2]];
        o[3] = ch == 4 ? p[3] * (1.0f / 255.0f) : 1.0f;
    } else {
        o[0] = o[1] = o[2] = tables->srgb_to_linear[p[0]];
        o[3] = ch == 2 ? p[1] * (1.0f / 255.0f) : 1.0f;
    }
}

static void core_image_store(const Core_ImageColorTables *tables, u8 *d, const f32 *v, f32 norm) {
    d[0] = core_image_to_srgb(tables, v[0] * norm);
    d[1] = core_image_to_srgb(tables, v[1] * norm);
    d[2] = core_image_to_srgb(tables, v[2] * norm);
    d[3] = (u8)CORE_CLAMP((s32)(v[3] * norm * 255.0f + 0.5f), 0, 255);
}

b32 core_image_resize_square(const Core_ImageColorTables *tables, const u8 *src,
                             u32 sw, u32 sh, u32 ch, u8 *dst, u32 size,
                             void *scratch, u32 scratch_capacity) {
    u64 scratch_bytes = (u64)scratch_capacity * CORE_IMAGE_SCRATCH_PER_PIXEL;
    if (!tables || !src || !dst || !sw || !sh || ch < 1 || ch > 4 || !size ||
        !scratch || size > scratch_capacity ||
        (usize)scratch % _Alignof(f32) || (u64)sw * sh > SIZE_MAX / ch ||
        (u64)size * size > SIZE_MAX / 4 || scratch_bytes > SIZE_MAX) return false;
    f32 *acc = scratch;
    u32 *xi0 = (u32 *)(acc + (u64)size * 4), *xi1 = xi0 + size;
    f32 *xw0 = (f32 *)(xi1 + size), *xw1 = xw0 + size, *xsum = xw1 + size;
    u32 side = CORE_MIN(sw, sh);
    f64 cx = (sw - side) / 2, cy = (sh - side) / 2;
    f64 scale = (f64)side / size;
    f32 px[4];

    if (scale < 1.0) {
        for (u32 oy = 0; oy < size; oy++) {
            f64 fy = CORE_CLAMP(cy + (oy + 0.5) * scale - 0.5, cy, cy + side - 1);
            u32 y0 = (u32)fy, y1 = CORE_MIN(y0 + 1, (u32)(cy + side - 1));
            f32 ty = (f32)(fy - y0);
            for (u32 ox = 0; ox < size; ox++) {
                f64 fx = CORE_CLAMP(cx + (ox + 0.5) * scale - 0.5, cx, cx + side - 1);
                u32 x0 = (u32)fx, x1 = CORE_MIN(x0 + 1, (u32)(cx + side - 1));
                f32 tx = (f32)(fx - x0), v[4] = {0}, a[4], b[4], cc[4], d[4];
                core_image_fetch(tables, src + ((u64)y0 * sw + x0) * ch, ch, a);
                core_image_fetch(tables, src + ((u64)y0 * sw + x1) * ch, ch, b);
                core_image_fetch(tables, src + ((u64)y1 * sw + x0) * ch, ch, cc);
                core_image_fetch(tables, src + ((u64)y1 * sw + x1) * ch, ch, d);
                for (u32 k = 0; k < 4; k++)
                    v[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (cc[k] * (1 - tx) + d[k] * tx) * ty;
                core_image_store(tables, dst + ((u64)oy * size + ox) * 4, v, 1.0f);
            }
        }
        return true;
    }

    for (u32 ox = 0; ox < size; ox++) {
        f64 x0 = cx + ox * scale, x1 = x0 + scale;
        u32 i0 = (u32)x0, i1 = (u32)x1;
        if ((f64)i1 >= x1) i1--;                  /* x1 exactly on a pixel edge */
        i1 = CORE_MIN(i1, sw - 1);
        xi0[ox] = i0;
        xi1[ox] = i1;
        if (i0 == i1) { xw0[ox] = (f32)(x1 - x0); xw1[ox] = 0; }
        else          { xw0[ox] = (f32)(i0 + 1 - x0); xw1[ox] = (f32)CORE_MIN(x1 - i1, 1.0); }
        xsum[ox] = xw0[ox] + xw1[ox] + (i1 > i0 ? (f32)(i1 - i0 - 1) : 0);
    }

    for (u32 oy = 0; oy < size; oy++) {
        f64 y0 = cy + oy * scale, y1 = y0 + scale;
        u32 j0 = (u32)y0, j1 = (u32)y1;
        if ((f64)j1 >= y1) j1--;
        j1 = CORE_MIN(j1, sh - 1);
        memset(acc, 0, sizeof(f32) * 4 * size);
        f32 wsum = 0;
        for (u32 sy = j0; sy <= j1; sy++) {
            f32 wy = (f32)(CORE_MIN(sy + 1.0, y1) - CORE_MAX((f64)sy, y0));
            if (wy <= 0) continue;
            wsum += wy;
            const u8 *row = src + (u64)sy * sw * ch;
            for (u32 ox = 0; ox < size; ox++) {
                u32 i0 = xi0[ox], i1 = xi1[ox];
                f32 s[4] = {0};
                for (u32 sx = i0; sx <= i1; sx++) {
                    f32 wx = sx == i0 ? xw0[ox] : (sx == i1 ? xw1[ox] : 1.0f);
                    core_image_fetch(tables, row + (u64)sx * ch, ch, px);
                    s[0] += px[0] * wx; s[1] += px[1] * wx; s[2] += px[2] * wx; s[3] += px[3] * wx;
                }
                f32 *out_acc = acc + (u64)ox * 4;
                out_acc[0] += s[0] * wy; out_acc[1] += s[1] * wy; out_acc[2] += s[2] * wy; out_acc[3] += s[3] * wy;
            }
        }
        for (u32 ox = 0; ox < size; ox++) {
            f32 norm = 1.0f / CORE_MAX(wsum * xsum[ox], 1e-6f);
            core_image_store(tables, dst + ((u64)oy * size + ox) * 4, acc + (u64)ox * 4, norm);
        }
    }
    return true;
}

