#include "core/image.c"
#include "core/memory.c" /* third-party allocator linkage */

#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "image test: %s at %d\n", #x, __LINE__); return 1; } } while (0)

int main(void) {
    Core_ImageColorTables tables;
    core_image_color_tables_init(&tables);
    _Alignas(f32) u8 scratch[4 * CORE_IMAGE_SCRATCH_PER_PIXEL];
    u8 dst[4 * 4 * 4];
    /* Constant images in all stb channel layouts, including grayscale alpha. */
    for (u32 ch = 1; ch <= 4; ch++) {
        u8 src[5 * 3 * 4];
        for (u32 p = 0; p < 15; p++) {
            for (u32 k = 0; k < ch; k++) src[p * ch + k] = 123;
            if (ch == 2 || ch == 4) src[p * ch + ch - 1] = 57;
        }
        for (u32 size = 1; size <= 4; size++) {
            CHECK(core_image_resize_square(&tables, src, 5, 3, ch, dst, size, scratch, 4));
            for (u32 p = 0; p < size * size; p++) {
                CHECK(abs((int)dst[p * 4] - 123) <= 1);
                CHECK(dst[p * 4] == dst[p * 4 + 1] && dst[p * 4] == dst[p * 4 + 2]);
                CHECK(dst[p * 4 + 3] == (ch == 2 || ch == 4 ? 57 : 255));
            }
        }
    }
    /* Equal black and white area should be ~188 sRGB, not the old 127. */
    u8 checker[] = {0, 255, 255, 0};
    CHECK(core_image_resize_square(&tables, checker, 2, 2, 1, dst, 1, scratch, 4));
    CHECK(dst[0] >= 187 && dst[0] <= 188 && dst[3] == 255);
    /* Bilinear center has the same linear-light average. */
    CHECK(core_image_resize_square(&tables, checker, 2, 2, 1, dst, 3, scratch, 4));
    CHECK(dst[(1 * 3 + 1) * 4] >= 187 && dst[(1 * 3 + 1) * 4] <= 188);
    CHECK(dst[0] == 0 && dst[8] == 255);
    /* Fractional footprint: each 1.5-wide cell covers 1/3 white and 2/3 black. */
    u8 stripes[] = {0,255,0, 0,255,0, 0,255,0};
    CHECK(core_image_resize_square(&tables, stripes, 3, 3, 1, dst, 2, scratch, 4));
    for (u32 p = 0; p < 4; p++) CHECK(dst[p * 4] >= 155 && dst[p * 4] <= 157);
    /* Integer centered crop discards outer columns. */
    u8 wide[] = {255,0,0,255, 255,0,0,255};
    CHECK(core_image_resize_square(&tables, wide, 4, 2, 1, dst, 1, scratch, 4));
    CHECK(dst[0] == 0);
    u8 alpha[] = {255,0, 255,64, 255,128, 255,255};
    CHECK(core_image_resize_square(&tables, alpha, 2, 2, 2, dst, 1, scratch, 4));
    CHECK(dst[0] == 255 && dst[3] == 112);
    memset(dst, 42, sizeof(dst));
    CHECK(!core_image_resize_square(&tables, checker, 0, 2, 1, dst, 1, scratch, 4));
    CHECK(!core_image_resize_square(&tables, checker, 2, 2, 5, dst, 1, scratch, 4));
    CHECK(!core_image_resize_square(&tables, checker, 2, 2, 1, dst, 4, scratch, 3));
    CHECK(!core_image_resize_square(&tables, checker, 2, 2, 1, dst, 1, scratch + 1, 4));
    CHECK(dst[0] == 42);
    puts("image_test: ok");
    return 0;
}
