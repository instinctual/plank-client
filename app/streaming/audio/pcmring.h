#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace PlankAudio {
// One decoded-audio producer and one realtime consumer. Neither may
// advance the other's cursor: overflow is backpressure, never silent eviction.
class PcmRing
{
public:
    PcmRing(unsigned capacity, unsigned channels)
        : m_Capacity(capacity), m_Channels(channels), m_Data(capacity * channels) {}

    std::uint64_t written() const { return m_Write.load(std::memory_order_acquire); }
    std::uint64_t read() const { return m_Read.load(std::memory_order_acquire); }
    unsigned queued() const {
        const auto r = read();
        return static_cast<unsigned>(std::min<std::uint64_t>(written() - r, m_Capacity));
    }
    bool push(const float* samples, unsigned frames) {
        const auto w = m_Write.load(std::memory_order_relaxed);
        if (frames > m_Capacity || w - read() > m_Capacity - frames) return false;
        copyIn(samples, w, frames);
        m_Write.store(w + frames, std::memory_order_release);
        return true;
    }
    unsigned pop(float* samples, unsigned frames) {
        const auto r = m_Read.load(std::memory_order_relaxed);
        frames = static_cast<unsigned>(std::min<std::uint64_t>(frames, written() - r));
        const unsigned offset = r % m_Capacity;
        const unsigned first = std::min(frames, m_Capacity - offset);
        std::memcpy(samples, m_Data.data() + offset * m_Channels, first * m_Channels * sizeof(float));
        std::memcpy(samples + first * m_Channels, m_Data.data(), (frames - first) * m_Channels * sizeof(float));
        m_Read.store(r + frames, std::memory_order_release);
        return frames;
    }
private:
    void copyIn(const float* samples, std::uint64_t w, unsigned frames) {
        const unsigned offset = w % m_Capacity;
        const unsigned first = std::min(frames, m_Capacity - offset);
        std::memcpy(m_Data.data() + offset * m_Channels, samples, first * m_Channels * sizeof(float));
        std::memcpy(m_Data.data(), samples + first * m_Channels, (frames - first) * m_Channels * sizeof(float));
    }
    const unsigned m_Capacity, m_Channels;
    std::vector<float> m_Data;
    alignas(64) std::atomic<std::uint64_t> m_Write {0};
    alignas(64) std::atomic<std::uint64_t> m_Read {0};
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "Realtime audio needs lock-free counters");
static_assert(std::atomic<int>::is_always_lock_free, "Realtime audio needs lock-free status");

}
