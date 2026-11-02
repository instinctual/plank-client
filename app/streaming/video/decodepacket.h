#pragma once

#include <Limelight.h>
#include <cstring>
#include <limits>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
}

// Keep Host parameter sets intact. The same bitstream qualified at startup is
// decoded during streaming; no Client-side SPS edits or IDR-only copy path.
inline bool plankSetDecodePacket(AVPacket* packet, const DECODE_UNIT* unit,
                                std::vector<uint8_t>& assembly)
{
    av_packet_unref(packet);
    if (!unit || unit->fullLength <= 0 ||
            unit->fullLength > std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE)
        return false;
    if (const auto* data = LiVideoFrameData(unit)) {
        static_assert(PLANK_VIDEO_BUFFER_PADDING >= AV_INPUT_BUFFER_PADDING_SIZE);
        LiRetainVideoBuffer(unit->frameBuffer);
        packet->buf = av_buffer_create(const_cast<uint8_t*>(data),
            unit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE,
            [](void* owner, uint8_t*) { LiReleaseVideoBuffer(static_cast<PLANK_VIDEO_BUFFER*>(owner)); },
            unit->frameBuffer, AV_BUFFER_FLAG_READONLY);
        if (!packet->buf) {
            LiReleaseVideoBuffer(unit->frameBuffer);
            return false;
        }
        packet->data = packet->buf->data;
    } else {
        assembly.resize(unit->fullLength + AV_INPUT_BUFFER_PADDING_SIZE);
        size_t offset = 0;
        for (auto* entry = unit->bufferList; entry; entry = entry->next) {
            if (!entry->data || entry->length <= 0 ||
                    size_t(entry->length) > size_t(unit->fullLength) - offset) return false;
            memcpy(assembly.data() + offset, entry->data, entry->length);
            offset += entry->length;
        }
        if (offset != size_t(unit->fullLength)) return false;
        memset(assembly.data() + offset, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        packet->data = assembly.data();
    }
    packet->size = unit->fullLength;
    if (unit->frameType == FRAME_TYPE_IDR) packet->flags = AV_PKT_FLAG_KEY;
    return true;
}
