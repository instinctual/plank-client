#pragma once

#include <Limelight.h>

// HEVC must pass the exact-profile hardware probe. H.264 profiles such as
// High 10 4:4:4 still need software decoding on otherwise qualified Clients.
// The Host encoder (x264, NVENC or VideoToolbox) does not define decoder support.
inline constexpr bool plankSoftwareDecodingAllowed(int videoFormat)
{
    switch (videoFormat) {
    case VIDEO_FORMAT_H264:
    case VIDEO_FORMAT_H264_HIGH8_422:
    case VIDEO_FORMAT_H264_HIGH10_422:
    case VIDEO_FORMAT_H264_HIGH8_444:
    case VIDEO_FORMAT_H264_HIGH10_444:
        return true;
    default:
        return false;
    }
}
