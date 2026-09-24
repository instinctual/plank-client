// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// UVC H.264 1.0 extension GUID and picture-type selector, as published by
// USB-IF in uvc_h264.h (also distributed with GStreamer's uvch264 plugin).
// Parse only the active configuration and this node's VideoControl interface;
// a vendor name or a familiar unit number never authorizes an extension write.
inline std::uint8_t plankUvcH264Unit(const std::uint8_t* bytes, std::size_t size,
                                   unsigned configuration, unsigned interface)
{
    constexpr std::uint8_t guid[] = {0x41,0x76,0x9e,0xa2,0x04,0xde,0xe3,0x47,
        0x8b,0x2b,0xf4,0x34,0x1a,0xff,0x00,0x3b};
    if (!bytes || !size || size > 65535 || !configuration || configuration > 255 || interface > 255) return 0;
    bool selectedConfiguration = false, selectedInterface = false;
    std::uint8_t unit = 0;
    for (std::size_t at = 0; at < size;) {
        if (size - at < 2) return 0;
        const std::size_t length = bytes[at];
        if (length < 2 || length > size - at) return 0;
        const auto* p = bytes + at;
        if (p[1] == 2) {
            if (length < 9) return 0;
            selectedConfiguration = p[5] == configuration;
            selectedInterface = false;
        } else if (p[1] == 4) {
            if (length < 9) return 0;
            selectedInterface = selectedConfiguration && p[2] == interface && !p[3] && p[5] == 14 && p[6] == 1;
        } else if (selectedInterface && p[1] == 0x24 && length >= 3 && p[2] == 6) {
            if (length < 24) return 0;
            if (!std::memcmp(p + 4, guid, sizeof(guid))) {
                const std::size_t pins = p[21];
                if (!pins || 23 + pins >= length) return 0;
                const std::size_t controls = p[22 + pins];
                if (controls < 2 || 24 + pins + controls != length || !(p[24 + pins] & 1) || !p[3] || unit) return 0;
                unit = p[3];
            }
        }
        at += length;
    }
    return unit;
}
