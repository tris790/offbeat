#ifndef CORE_IMAGE_H
#define CORE_IMAGE_H

#include "types.h"

/* Initialize once before starting workers; then share read-only. */
typedef struct {
    f32 srgb_to_linear[256];
    u8 linear_to_srgb[4096];
} Core_ImageColorTables;
void core_image_color_tables_init(Core_ImageColorTables *tables);

/* Center-crop a tightly packed 1..4-channel sRGB image to a square RGBA8 image.
   Exact area coverage when shrinking; bilinear when enlarging. RGB averages
   in linear light; alpha is averaged separately (straight, not premultiplied).
   Source/destination/scratch must not overlap. Scratch is f32-aligned and holds
   scratch_capacity * CORE_IMAGE_SCRATCH_PER_PIXEL bytes. No heap allocations.
   Returns false for invalid dimensions/channels/scratch, without writing dst. */
#define CORE_IMAGE_SCRATCH_PER_PIXEL 36
b32 core_image_resize_square(const Core_ImageColorTables *tables, const u8 *src,
                             u32 width, u32 height, u32 channels, u8 *dst,
                             u32 size, void *scratch, u32 scratch_capacity);

#endif
