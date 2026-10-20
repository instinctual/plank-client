#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace PlankPipeWire {

// One decoded-audio producer and one PipeWire realtime consumer. Neither may
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

struct OutputClock {
    std::uint64_t observedNs = 0;
    std::uint64_t tailNs = 0; // presentation end of the buffer just queued
    std::uint64_t consumed = 0;
    std::uint64_t pulls = 0;
    std::uint64_t underruns = 0;
    std::uint64_t missingFrames = 0;
    unsigned quantum = 0;
    unsigned headroom = 0;
    bool priming = true;
};

// All fields are atomic: a conventional seqlock over an ordinary C++ struct
// would still be a data race. The reader never delays the realtime writer.
class ClockSnapshot
{
public:
    void publish(const OutputClock& c) {
        m_Sequence.fetch_add(1, std::memory_order_seq_cst);
        const std::array<std::uint64_t, 9> values {c.observedNs, c.tailNs, c.consumed,
            c.pulls, c.underruns, c.missingFrames, c.quantum, c.headroom, c.priming};
        for (unsigned i = 0; i < values.size(); ++i) m_Values[i].store(values[i]);
        m_Sequence.fetch_add(1, std::memory_order_seq_cst);
    }
    OutputClock get() const {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const auto before = m_Sequence.load();
            if (before & 1) continue;
            std::array<std::uint64_t, 9> v {};
            for (unsigned i = 0; i < v.size(); ++i) v[i] = m_Values[i].load();
            if (before == m_Sequence.load())
                return {v[0], v[1], v[2], v[3], v[4], v[5],
                    static_cast<unsigned>(v[6]), static_cast<unsigned>(v[7]), v[8] != 0};
        }
        return {}; // invalid timing disables phase correction for this block
    }
private:
    std::atomic<std::uint64_t> m_Sequence {0};
    std::array<std::atomic<std::uint64_t>, 9> m_Values {};
};

// PipeWire time.queued is in application pw_buffer.size units (frames here).
// delay is graph-clock ticks, buffered is stream frames. No fixed device
// quantum is added: that would double-count hardware latency.
inline std::uint64_t presentationTailNs(std::int64_t clockNs, std::int64_t delay,
    unsigned rateNum, unsigned rateDenom, std::uint64_t queued,
    std::uint64_t buffered, unsigned sampleRate, std::uint64_t nowNs)
{
    if (clockNs <= 0 || !rateNum || !rateDenom || !sampleRate ||
        queued > 2 * sampleRate || buffered > 2 * sampleRate) return 0;
    const long double latency = std::max<std::int64_t>(0, delay) *
        (1000000000.0L * rateNum / rateDenom) +
        (queued + buffered) * (1000000000.0L / sampleRate);
    const long double tail = clockNs + latency;
    if (latency > 2000000000.0L || tail < nowNs - std::min(nowNs, std::uint64_t(2000000000)) ||
        tail > nowNs + 2000000000.0L) return 0;
    return static_cast<std::uint64_t>(tail);
}

inline std::int64_t queueDelayUs(const OutputClock& clock, std::uint64_t written,
    std::uint64_t nowNs, unsigned rate)
{
    if (!rate || !clock.observedNs || !clock.tailNs || clock.priming ||
        nowNs < clock.observedNs || nowNs - clock.observedNs > 100000000 ||
        written < clock.consumed || written - clock.consumed > 2 * rate) return -1;
    const auto ringUs = (written - clock.consumed) * 1000000 / rate;
    // A captured read cursor keeps queue + device accounting consistent even
    // if another process callback moves samples from the ring in the meantime.
    return std::max<std::int64_t>(0, static_cast<std::int64_t>(ringUs) +
        (static_cast<std::int64_t>(clock.tailNs) - static_cast<std::int64_t>(nowNs)) / 1000);
}

constexpr int ReserveUs = 12000; // covers the Host's 512/48k capture batch

inline unsigned producerHighWater(unsigned rate, unsigned quantum)
{
    // A large graph pull must be allowed to prime and retain its reserve.
    // A fixed 50ms ceiling would starve devices requesting 2048/4096 frames.
    return std::max(rate / 20, quantum + rate * ReserveUs / 1000000);
}

// Producer-side safety ceiling, driven by the minimum POST-pull reserve over
// one second, not the misleading pre-enqueue fill. Smooth resampling remains
// the only clock actuator. Never accelerate repeatedly into an empty queue.
class HeadroomGuard
{
public:
    int apply(int requestedPpm, int minimumUs, std::uint64_t nowMs, bool healthy) {
        if (!healthy) {
            m_Minima.fill(-1);
            m_LastBucket = nowMs / 100;
            m_Applied = 0;
            return 0;
        }
        const auto bucket = nowMs / 100;
        if (bucket - m_LastBucket >= m_Minima.size()) m_Minima.fill(-1);
        else for (auto n = m_LastBucket + 1; n <= bucket; ++n) m_Minima[n % m_Minima.size()] = -1;
        m_LastBucket = bucket;
        auto& value = m_Minima[bucket % m_Minima.size()];
        if (minimumUs >= 0) value = value < 0 ? minimumUs : std::min(value, minimumUs);
        int low = std::numeric_limits<int>::max();
        for (int item : m_Minima) if (item >= 0) low = std::min(low, item);
        if (low == std::numeric_limits<int>::max()) return m_Applied = 0;
        const int ceiling = std::clamp((low - ReserveUs) / 5, -10000, 10000);
        const int target = std::min(requestedPpm, ceiling);
        // Stop acceleration immediately; ramp all other rate changes over time.
        if (m_Applied > 0 && target <= 0) m_Applied = 0;
        const int step = static_cast<int>(std::min<std::uint64_t>(nowMs - m_LastUpdate, 100) * 10);
        m_LastUpdate = nowMs;
        m_Applied += std::clamp(target - m_Applied, -step, step);
        return m_Applied;
    }
private:
    std::array<int, 10> m_Minima {{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}};
    std::uint64_t m_LastBucket = 0, m_LastUpdate = 0;
    int m_Applied = 0;
};

// Consumer-only state. A short read is counted, with a short fade at the
// discontinuity; it is not hidden as a successful full buffer. Re-prime after
// a real interruption instead of alternating tiny fragments and silence.
class Playback
{
public:
    Playback(PcmRing& ring, unsigned rate, unsigned channels)
        : m_Ring(ring), m_Rate(rate), m_Channels(channels) {}
    unsigned render(float* output, unsigned frames) {
        std::fill_n(output, frames * m_Channels, 0.0f);
        m_Clock.quantum = frames;
        ++m_Clock.pulls;
        if (m_Clock.priming && m_Ring.queued() < frames + m_Rate * ReserveUs / 1000000) {
            // Do not count intentional initial priming as an underrun. Once
            // playback has started, include all recovery silence in the total.
            if (m_Started) m_Clock.missingFrames += frames;
            m_Clock.headroom = m_Ring.queued();
            return 0;
        }
        if (m_Clock.priming) m_FadeIn = std::max(1U, m_Rate / 400);
        m_Clock.priming = false;
        m_Started = true;
        const unsigned read = m_Ring.pop(output, frames);
        const unsigned fade = std::max(1U, m_Rate / 400); // 2.5ms only at start/recovery
        for (unsigned f = 0; f < read && m_FadeIn; ++f, --m_FadeIn)
            for (unsigned c = 0; c < m_Channels; ++c)
                output[f * m_Channels + c] *= float(fade - m_FadeIn + 1) / fade;
        if (read < frames) {
            ++m_Clock.underruns;
            m_Clock.missingFrames += frames - read;
            const unsigned count = std::min(read, fade);
            for (unsigned f = 0; f < count; ++f)
                for (unsigned c = 0; c < m_Channels; ++c)
                    output[(read - count + f) * m_Channels + c] *= float(count - f - 1) / count;
            m_Clock.priming = true;
        }
        m_Clock.consumed = m_Ring.read();
        m_Clock.headroom = m_Ring.queued();
        return read;
    }
    OutputClock& clock() { return m_Clock; }
private:
    PcmRing& m_Ring;
    const unsigned m_Rate, m_Channels;
    unsigned m_FadeIn = 0;
    bool m_Started = false;
    OutputClock m_Clock;
};

} // namespace PlankPipeWire
