// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace PlankNativeCameraFrame {
constexpr std::size_t MaxBytes = 4 * 1024 * 1024;

inline std::size_t startCode(const std::uint8_t* bytes, std::size_t size, std::size_t at)
{
    if (at <= size && size - at >= 3 && !bytes[at] && !bytes[at + 1]) {
        if (bytes[at + 2] == 1) return 3;
        if (size - at >= 4 && !bytes[at + 2] && bytes[at + 3] == 1) return 4;
    }
    return 0;
}

// Bounded framing classification, not a codec decoder. The Host must validate
// SPS dimensions, parameter-set references and decoding independently. A
// recovery frame must carry its own SPS/PPS; never splice old sets into it.
inline bool h264(const std::uint8_t* bytes, std::size_t size, bool& independent)
{
    independent = false;
    if (!bytes || !size || size > MaxBytes) return false;
    std::size_t at = 0;
    unsigned count = 0;
    bool sps = false, pps = false, idr = false, dependent = false;
    while (at < size) {
        const auto prefix = startCode(bytes, size, at);
        if (!prefix || ++count > 64) return false;
        const auto begin = at + prefix;
        auto end = begin;
        while (end < size && !startCode(bytes, size, end)) ++end;
        at = end;
        while (end > begin && !bytes[end - 1]) --end;
        if (end - begin < 2 || (bytes[begin] & 0x80)) return false;
        const unsigned type = bytes[begin] & 31;
        if (!type || type > 12 || (type >= 2 && type <= 4)) return false;
        if (type == 7 || type == 8) {
            if (end - begin > 4096 || idr || dependent) return false;
            if (type == 7) sps = true; else pps = true;
        }
        if (type == 5) idr = true;
        if (type == 1) dependent = true;
    }
    independent = sps && pps && idr;
    return (idr || dependent) && !(idr && dependent);
}

// Accept baseline JPEG framing and exact SOF dimensions. Entropy decoding and
// color interpretation still belong to the Host. No payload bytes are changed.
inline bool mjpeg(const std::uint8_t* bytes, std::size_t size, unsigned width, unsigned height)
{
    if (!bytes || size < 4 || size > MaxBytes || bytes[0] != 0xff || bytes[1] != 0xd8) return false;
    std::size_t at = 2;
    bool sof = false, scan = false;
    unsigned markers = 0;
    while (at < size) {
        if (scan && bytes[at] != 0xff) { ++at; continue; }
        if (bytes[at++] != 0xff || at == size) return false;
        while (at < size && bytes[at] == 0xff) ++at;
        if (at == size) return false;
        const unsigned marker = bytes[at++];
        if (scan && (!marker || (marker >= 0xd0 && marker <= 0xd7))) continue;
        if (marker == 0xd9) return sof && scan && at == size;
        if (++markers > 256 || scan || !marker || marker == 0xd8 ||
            (marker >= 0xd0 && marker <= 0xd7) || size - at < 2) return false;
        const std::size_t length = (unsigned(bytes[at]) << 8) | bytes[at + 1];
        if (length < 2 || length > size - at) return false;
        if (marker == 0xc0) {
            if (sof || length < 8 || bytes[at + 2] != 8 ||
                ((unsigned(bytes[at + 3]) << 8) | bytes[at + 4]) != height ||
                ((unsigned(bytes[at + 5]) << 8) | bytes[at + 6]) != width ||
                (bytes[at + 7] != 1 && bytes[at + 7] != 3) ||
                length != 8u + 3u * bytes[at + 7]) return false;
            sof = true;
        } else if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4) {
            return false;
        } else if (marker == 0xda) {
            if (!sof || length < 6 || !bytes[at + 2] || bytes[at + 2] > 3 ||
                length != 6u + 2u * bytes[at + 2]) return false;
            scan = true;
        }
        at += length;
    }
    return false;
}
}
