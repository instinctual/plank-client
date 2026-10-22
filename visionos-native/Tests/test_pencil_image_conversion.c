// SPDX-License-Identifier: GPL-3.0-or-later
#include "PlankPencilImageConversion.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
static void check(int condition) { ++checks; assert(condition); }
static void word(uint8_t *p, uint16_t value) { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); }

static void padded_bgra_rounding(void) {
    uint8_t source[42], destination[8];
    memset(source, 231, sizeof source);
    for (unsigned y = 0; y < 2; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            const unsigned value = x < 2 ? x + y * 2 : 20 + (x - 2) + y * 2;
            uint8_t *p = source + y * 21 + x * 4;
            p[0] = value; p[1] = value + 10; p[2] = value + 20;
            p[3] = x * 64; // Preserve premultiplied channels; do not unpremultiply.
        }
    }
    const uint8_t expected[] = {2,12,22,255, 22,32,42,255};
    check(plank_pencil_image_convert_bgra8(source, sizeof source, 21, 4, 2, destination, sizeof destination, 2, 1));
    check(memcmp(destination, expected, sizeof expected) == 0); // .5 ties away from zero.
    for (unsigned y = 0; y < 2; ++y)
        check(source[y * 21 + 16] == 231 && source[y * 21 + 20] == 231);
}

static void padded_little_endian_gbr(void) {
    uint8_t green_storage[11], br_storage[19], destination[16];
    memset(green_storage, 165, sizeof green_storage); memset(br_storage, 165, sizeof br_storage);
    uint8_t *green = green_storage + 1, *br = br_storage + 1; // Deliberately unaligned bases and strides.
    const uint16_t g[] = {32736,0,16448,65472}, b[] = {64,65535,49216,32736}, r[] = {65472,16448,0,65535};
    for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 2; ++x) {
        const unsigned p = y * 2 + x;
        word(green + y * 5 + x * 2, g[p]);
        word(br + y * 9 + x * 4, b[p]); word(br + y * 9 + x * 4 + 2, r[p]);
    }
    const uint8_t expected[] = {0,128,255,255, 255,0,64,255, 192,64,0,255, 128,255,255,255};
    check(plank_pencil_image_convert_gbr10(green, 10, 5, br, 18, 9, 2, 2, destination, sizeof destination, 2, 2));
    // 32736 retains lower word bits and rounds 127.5 to128; silently masking
    // would instead produce127. High words saturate before interpolation.
    check(memcmp(destination, expected, sizeof expected) == 0);
    check(green_storage[0] == 165 && green[4] == 165 && green[9] == 165);
    check(br_storage[0] == 165 && br[8] == 165 && br[17] == 165);
    uint8_t average[4];
    const uint8_t expected_average[] = {144,112,144,255};
    check(plank_pencil_image_convert_gbr10(green, 10, 5, br, 18, 9, 2, 2, average, sizeof average, 1, 1));
    check(memcmp(average, expected_average, sizeof average) == 0);
}

static void invalid_storage(void) {
    uint8_t source[16] = {0}, green[8] = {0}, destination[16];
    memset(destination, 107, sizeof destination);
#define BGRA(S,L,T,SW,SH,D,DL,DW,DH) check(!plank_pencil_image_convert_bgra8(S,L,T,SW,SH,D,DL,DW,DH))
    BGRA(NULL,16,8,2,2,destination,16,2,2);
    BGRA(source,16,8,2,2,NULL,16,2,2);
    BGRA(source,15,8,2,2,destination,16,2,2);
    BGRA(source,16,7,2,2,destination,16,2,2);
    BGRA(source,16,8,2,2,destination,15,2,2);
    BGRA(source,16,8,0,2,destination,16,2,2);
    BGRA(source,16,8,2,0,destination,16,2,2);
    BGRA(source,16,8,2,2,destination,16,0,2);
    BGRA(source,16,8,2,2,destination,16,2,0);
    BGRA(source,SIZE_MAX,SIZE_MAX,2,2,destination,16,2,2);
    BGRA(source,SIZE_MAX,268435457,2,1,destination,16,2,1);
    BGRA(source,SIZE_MAX,65540,16385,1,destination,16,2,1);
    BGRA(source,SIZE_MAX,65536,16384,2049,destination,16,2,1);
    BGRA(source,SIZE_MAX,7684,1921,1,destination,SIZE_MAX,1921,1);
    BGRA(source,16,8,2,2,destination,16,3,2);
    BGRA(source,16,8,2,2,destination,16,2,3);
#undef BGRA
#define GBR(G,GL,GS,BR,BL,BS,SW,SH,D,DL,DW,DH) check(!plank_pencil_image_convert_gbr10(G,GL,GS,BR,BL,BS,SW,SH,D,DL,DW,DH))
    GBR(NULL,8,4,source,16,8,2,2,destination,16,2,2);
    GBR(green,8,4,NULL,16,8,2,2,destination,16,2,2);
    GBR(green,8,4,source,16,8,2,2,NULL,16,2,2);
    GBR(green,7,4,source,16,8,2,2,destination,16,2,2);
    GBR(green,8,3,source,16,8,2,2,destination,16,2,2);
    GBR(green,8,4,source,15,8,2,2,destination,16,2,2);
    GBR(green,8,4,source,16,7,2,2,destination,16,2,2);
    GBR(green,8,4,source,16,8,2,2,destination,15,2,2);
    GBR(green,SIZE_MAX,SIZE_MAX,source,SIZE_MAX,SIZE_MAX,2,2,destination,16,2,2);
    GBR(green,SIZE_MAX,134217730,source,SIZE_MAX,134217732,2,1,destination,16,2,1);
    GBR(green,8,4,source,16,8,2,2,destination,16,0,2);
    GBR(green,8,4,source,16,8,0,2,destination,16,2,2);
#undef GBR
    for (unsigned i = 0; i < sizeof destination; ++i) check(destination[i] == 107);
}

int main(void) {
    padded_bgra_rounding(); padded_little_endian_gbr(); invalid_storage();
    printf("Pencil image native conversion: %u checks passed\n", checks);
    return 0;
}
