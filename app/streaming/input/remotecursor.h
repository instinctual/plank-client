#pragma once

#include <Limelight.h>
#include <plank.h>
#include <QSize>
#include <QtEndian>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

// Shared cursor wire validation and bounded delivery. The transport thread
// assembles at most one image and coalesces at most one completed image and
// position. The UI owns presentation, never protocol parsing or sequence state.
class PlankRemoteCursor
{
public:
    struct Shape {
        std::uint64_t generation = 0;
        std::uint32_t width = 0, height = 0, hotspotX = 0, hotspotY = 0;
        std::uint32_t flags = 0, nextOffset = 0;
        std::vector<unsigned char> pixels;
    };
    struct Position {
        std::uint64_t sequence = 0;
        std::uint32_t x = 0, y = 0, frameWidth = 0, frameHeight = 0;
    };

    // True requests a single UI notification; subsequent submissions replace
    // the pending value until takeShape/takePosition consumes it.
    bool shape(const unsigned char* data, unsigned int length)
    {
        if (!data || length < sizeof(PLANK_CURSOR_WIRE_HEADER) ||
            length > sizeof(PLANK_CURSOR_WIRE_HEADER) + PLANK_CURSOR_MAX_CHUNK_SIZE) return false;
        PLANK_CURSOR_WIRE_HEADER wire;
        std::memcpy(&wire, data, sizeof(wire));
        Shape next;
        next.generation = qFromLittleEndian(wire.generation);
        next.width = qFromLittleEndian(wire.width); next.height = qFromLittleEndian(wire.height);
        next.hotspotX = qFromLittleEndian(wire.hotspotX); next.hotspotY = qFromLittleEndian(wire.hotspotY);
        const auto flags = qFromLittleEndian(wire.flags);
        next.flags = flags & PLANK_CURSOR_FLAG_VISIBLE;
        const auto size = qFromLittleEndian(wire.imageSize);
        const auto offset = qFromLittleEndian(wire.chunkOffset);
        const auto count = qFromLittleEndian(wire.chunkSize);
        constexpr auto known = PLANK_CURSOR_FLAG_VISIBLE | PLANK_CURSOR_FLAG_FIRST_CHUNK | PLANK_CURSOR_FLAG_LAST_CHUNK;
        if (qFromLittleEndian(wire.magic) != PLANK_CURSOR_WIRE_MAGIC ||
            qFromLittleEndian(wire.version) != PLANK_CURSOR_WIRE_VERSION ||
            qFromLittleEndian(wire.pixelFormat) != PLANK_CURSOR_PIXEL_FORMAT_ARGB8888 ||
            (flags & ~known) || !next.width || !next.height ||
            next.width > PLANK_CURSOR_MAX_DIMENSION || next.height > PLANK_CURSOR_MAX_DIMENSION ||
            size != next.width * next.height * 4U || size > PLANK_CURSOR_MAX_IMAGE_SIZE ||
            next.hotspotX >= next.width || next.hotspotY >= next.height ||
            count > PLANK_CURSOR_MAX_CHUNK_SIZE || sizeof(wire) + count != length ||
            offset > size || count > size - offset) return false;
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (flags & PLANK_CURSOR_FLAG_FIRST_CHUNK) {
            if (offset) return false;
            m_Assembly = std::move(next);
            m_Assembly.pixels.resize(size);
            m_Assembling = true;
        }
        const auto& a = m_Assembly;
        if (!m_Assembling || a.generation != next.generation || a.width != next.width || a.height != next.height ||
            a.hotspotX != next.hotspotX || a.hotspotY != next.hotspotY || a.flags != next.flags || a.nextOffset != offset) {
            m_Assembling = false;
            return false;
        }
        std::memcpy(m_Assembly.pixels.data() + offset, data + sizeof(wire), count);
        m_Assembly.nextOffset += count;
        if (!(flags & PLANK_CURSOR_FLAG_LAST_CHUNK)) return false;
        m_Assembling = false;
        if (m_Assembly.nextOffset != size) return false;
        const bool notify = !m_Shape.has_value();
        m_Shape = std::move(m_Assembly);
        return notify;
    }

    bool position(const unsigned char* data, unsigned int length, QSize stream)
    {
        if (!data || length != sizeof(PLANK_CURSOR_POSITION_WIRE_MESSAGE)) return false;
        PLANK_CURSOR_POSITION_WIRE_MESSAGE wire;
        std::memcpy(&wire, data, sizeof(wire));
        Position next{qFromLittleEndian(wire.sequence), qFromLittleEndian(wire.x), qFromLittleEndian(wire.y),
                      qFromLittleEndian(wire.frameWidth), qFromLittleEndian(wire.frameHeight)};
        if (qFromLittleEndian(wire.magic) != PLANK_CURSOR_POSITION_WIRE_MAGIC ||
            qFromLittleEndian(wire.version) != PLANK_CURSOR_POSITION_WIRE_VERSION || wire.reserved ||
            !next.sequence || !stream.isValid() || next.x >= next.frameWidth || next.y >= next.frameHeight ||
            next.frameWidth != std::uint32_t(stream.width()) || next.frameHeight != std::uint32_t(stream.height())) return false;
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (next.sequence <= m_HighestPosition) return false;
        m_HighestPosition = next.sequence;
        const bool notify = !m_Position.has_value();
        m_Position = next;
        return notify;
    }

    std::optional<Shape> takeShape()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto result = std::move(m_Shape);
        m_Shape.reset();
        return result;
    }
    std::optional<Position> takePosition()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto result = m_Position;
        m_Position.reset();
        return result;
    }
    // Call after the old transport's receive callbacks have stopped. Each new
    // login/desktop worker owns an independent position sequence beginning at 1.
    void resetPositionEpoch()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Position.reset();
        m_HighestPosition = 0;
    }
    void reset()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Assembly = {};
        m_Shape.reset(); m_Position.reset();
        m_Assembling = false; m_HighestPosition = 0;
    }
private:
    std::mutex m_Mutex;
    Shape m_Assembly;
    std::optional<Shape> m_Shape;
    std::optional<Position> m_Position;
    bool m_Assembling = false;
    std::uint64_t m_HighestPosition = 0;
};
