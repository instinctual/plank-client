#pragma once

#include <cstdint>

// The storage interpretation used by both the established EGL renderer and
// the Qt experiment. These are code values, not sRGB-linearized textures.
namespace PlankPackedVideo {
enum class Mapping { Unsupported, Identity8, Identity10, Bt709Full10 };

constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return uint32_t(a) | uint32_t(b) << 8 | uint32_t(c) << 16 | uint32_t(d) << 24;
}

constexpr uint32_t importFormat(uint32_t format, bool identity, bool bt709)
{
    // AYUV/XYUV store V,U,Y in the low-to-high bytes. XR24 sampling returns
    // Y,U,V, so the shader must reorder to V,Y,U for identity RGB.
    if (identity && (format == fourcc('A','Y','U','V') || format == fourcc('X','Y','U','V')))
        return fourcc('X','R','2','4');
    // Y410 stores U,Y,V in the low-to-high ten-bit words. XR30 returns V,Y,U:
    // already RGB for identity, or the input to our explicit BT.709 transform.
    if ((identity || bt709) && format == fourcc('Y','4','1','0'))
        return fourcc('X','R','3','0');
    return format;
}

constexpr Mapping mapping(uint32_t format, int bits, bool identity, bool bt709, bool fullRange)
{
    if (!fullRange || identity == bt709) return Mapping::Unsupported;
    if (bits == 10 && format == fourcc('Y','4','1','0'))
        return identity ? Mapping::Identity10 : Mapping::Bt709Full10;
    if (identity && bits == 8 &&
            (format == fourcc('A','Y','U','V') || format == fourcc('X','Y','U','V')))
        return Mapping::Identity8;
    return Mapping::Unsupported;
}
}
