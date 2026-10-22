// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PLANK_PENCIL_IMAGE_CONVERSION_H
#define PLANK_PENCIL_IMAGE_CONVERSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bounded, allocation-free bilinear conversion into tightly packed opaque BGRA.
// The source is the complete image, with the same pixel-centre transform as the
// Swift reference codec. Return 1 on success, 0 for invalid storage/dimensions.
// The caller retains its read-only pixel-buffer lock for the duration of GBR10.
int plank_pencil_image_convert_bgra8(
    const uint8_t *source, size_t source_length, size_t source_stride,
    uint32_t source_width, uint32_t source_height,
    uint8_t *destination, size_t destination_length,
    uint32_t destination_width, uint32_t destination_height);

// xf44 identity GBR10 only: plane 0 is little-endian G, plane 1 contains
// little-endian B then R. Raw words normalize by 65472, as in the Metal path.
// Premultiplied software RGB is preserved; neither path performs YUV conversion.
int plank_pencil_image_convert_gbr10(
    const uint8_t *green, size_t green_length, size_t green_stride,
    const uint8_t *blue_red, size_t blue_red_length, size_t blue_red_stride,
    uint32_t source_width, uint32_t source_height,
    uint8_t *destination, size_t destination_length,
    uint32_t destination_width, uint32_t destination_height);

#ifdef __cplusplus
}
#endif
#endif
