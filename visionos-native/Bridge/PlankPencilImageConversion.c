// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlankPencilImageConversion.h"
#include <math.h>

enum { maximum_source_dimension = 16384, maximum_destination_dimension = 1920 };
static const size_t maximum_source_pixels = 33554432;
static const size_t maximum_source_storage = 256 * 1024 * 1024;

typedef struct { double b, g, r; } Pixel;

static int dimensions_valid(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh) {
    return sw > 0 && sh > 0 && sw <= maximum_source_dimension && sh <= maximum_source_dimension &&
        (size_t)sw <= maximum_source_pixels / sh && dw > 0 && dh > 0 &&
        dw <= maximum_destination_dimension && dh <= maximum_destination_dimension && dw <= sw && dh <= sh;
}

static int storage_valid(size_t stride, size_t length, uint32_t width, uint32_t height, size_t bytes_per_pixel) {
    // Width/height have already passed dimensions_valid. Check division before
    // multiplying even when a hostile caller supplies SIZE_MAX as its stride.
    return stride >= (size_t)width * bytes_per_pixel &&
        stride <= maximum_source_storage / height && stride * height <= length;
}

static int destination_valid(const uint8_t *destination, size_t length, uint32_t width, uint32_t height) {
    // Destination dimensions are <=1920, so the tightly packed product is bounded.
    return destination != NULL && (size_t)width * height * 4 <= length;
}

static inline Pixel bgra_sample(const uint8_t *row, uint32_t x) {
    const uint8_t *p = row + (size_t)x * 4;
    // Keep premultiplied channels over black and replace only alpha with opaque.
    return (Pixel){p[0], p[1], p[2]};
}

static inline double word_channel(const uint8_t *p) {
    // Byte loads permit independently padded/unaligned plane strides. No mask
    // or YUV transform: preserve the Swift/Metal identity GBR normalization.
    const uint16_t word = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    const double value = (double)word * 255.0 / 65472.0;
    return value > 255.0 ? 255.0 : value;
}

static inline Pixel gbr_sample(const uint8_t *green_row, const uint8_t *blue_red_row, uint32_t x) {
    const uint8_t *br = blue_red_row + (size_t)x * 4;
    return (Pixel){word_channel(br), word_channel(green_row + (size_t)x * 2), word_channel(br + 2)};
}

static inline double interpolate(double a, double b, double c, double d, double horizontal, double vertical) {
    const double upper = a * (1.0 - horizontal) + b * horizontal;
    const double lower = c * (1.0 - horizontal) + d * horizontal;
    return upper * (1.0 - vertical) + lower * vertical;
}

static inline void write_pixel(uint8_t *out, Pixel a, Pixel b, Pixel c, Pixel d, double horizontal, double vertical) {
    // C round and Swift .rounded() both round ties away from zero. Per-source
    // compilation disables contraction/fast-math to preserve this operation order.
    out[0] = (uint8_t)round(interpolate(a.b, b.b, c.b, d.b, horizontal, vertical));
    out[1] = (uint8_t)round(interpolate(a.g, b.g, c.g, d.g, horizontal, vertical));
    out[2] = (uint8_t)round(interpolate(a.r, b.r, c.r, d.r, horizontal, vertical));
    out[3] = 255;
}

static inline double source_coordinate(uint32_t destination, double scale, uint32_t extent) {
    const double value = ((double)destination + 0.5) * scale - 0.5;
    return value < 0 ? 0 : (value > (double)(extent - 1) ? (double)(extent - 1) : value);
}

int plank_pencil_image_convert_bgra8(
    const uint8_t *source, size_t source_length, size_t source_stride,
    uint32_t sw, uint32_t sh, uint8_t *destination, size_t destination_length, uint32_t dw, uint32_t dh) {
    if (!dimensions_valid(sw, sh, dw, dh) || source == NULL ||
        !storage_valid(source_stride, source_length, sw, sh, 4) ||
        !destination_valid(destination, destination_length, dw, dh)) return 0;
    const double scale_x = (double)sw / dw, scale_y = (double)sh / dh;
    for (uint32_t y = 0; y < dh; ++y) {
        const double sy = source_coordinate(y, scale_y, sh);
        const uint32_t top = (uint32_t)sy, bottom = top + 1 < sh ? top + 1 : sh - 1;
        const double vertical = sy - top;
        const uint8_t *upper = source + (size_t)top * source_stride;
        const uint8_t *lower = source + (size_t)bottom * source_stride;
        uint8_t *out = destination + (size_t)y * dw * 4;
        for (uint32_t x = 0; x < dw; ++x, out += 4) {
            const double sx = source_coordinate(x, scale_x, sw);
            const uint32_t left = (uint32_t)sx, right = left + 1 < sw ? left + 1 : sw - 1;
            write_pixel(out, bgra_sample(upper, left), bgra_sample(upper, right),
                        bgra_sample(lower, left), bgra_sample(lower, right), sx - left, vertical);
        }
    }
    return 1;
}

int plank_pencil_image_convert_gbr10(
    const uint8_t *green, size_t green_length, size_t green_stride,
    const uint8_t *blue_red, size_t blue_red_length, size_t blue_red_stride,
    uint32_t sw, uint32_t sh, uint8_t *destination, size_t destination_length, uint32_t dw, uint32_t dh) {
    if (!dimensions_valid(sw, sh, dw, dh) || green == NULL || blue_red == NULL ||
        !storage_valid(green_stride, green_length, sw, sh, 2) ||
        !storage_valid(blue_red_stride, blue_red_length, sw, sh, 4) ||
        green_stride * sh > maximum_source_storage - blue_red_stride * sh ||
        !destination_valid(destination, destination_length, dw, dh)) return 0;
    const double scale_x = (double)sw / dw, scale_y = (double)sh / dh;
    for (uint32_t y = 0; y < dh; ++y) {
        const double sy = source_coordinate(y, scale_y, sh);
        const uint32_t top = (uint32_t)sy, bottom = top + 1 < sh ? top + 1 : sh - 1;
        const double vertical = sy - top;
        const uint8_t *green_upper = green + (size_t)top * green_stride;
        const uint8_t *green_lower = green + (size_t)bottom * green_stride;
        const uint8_t *br_upper = blue_red + (size_t)top * blue_red_stride;
        const uint8_t *br_lower = blue_red + (size_t)bottom * blue_red_stride;
        uint8_t *out = destination + (size_t)y * dw * 4;
        for (uint32_t x = 0; x < dw; ++x, out += 4) {
            const double sx = source_coordinate(x, scale_x, sw);
            const uint32_t left = (uint32_t)sx, right = left + 1 < sw ? left + 1 : sw - 1;
            write_pixel(out, gbr_sample(green_upper, br_upper, left), gbr_sample(green_upper, br_upper, right),
                        gbr_sample(green_lower, br_lower, left), gbr_sample(green_lower, br_lower, right), sx - left, vertical);
        }
    }
    return 1;
}
