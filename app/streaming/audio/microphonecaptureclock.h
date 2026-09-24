#pragma once
#include <cstdint>
#include <limits>

// PipeWire cycle timestamps and V4L2 camera timestamps both use CLOCK_MONOTONIC.
// Subtract the graph's reported capture latency and resampler backlog, never
// the time at which our polling/encoding thread happens to consume the data.
inline std::uint64_t plankMicrophoneCaptureTime(std::uint64_t cycle,
        std::int64_t delay, std::uint32_t rateNumerator, std::uint32_t rateDenominator,
        std::uint64_t buffered, std::uint64_t now)
{
    constexpr std::uint64_t second = 1000000000;
    if (!cycle || cycle > now || now - cycle > second / 10 ||
            rateNumerator != 1 || !rateDenominator || rateDenominator > 768000 ||
            buffered > 4800 || delay > std::int64_t(rateDenominator)) return 0;
    // PipeWire permits negative administrator offsets; clamp them as documented.
    const auto latency = (delay > 0 ? std::uint64_t(delay) * second / rateDenominator : 0) +
                         buffered * second / 48000;
    if (latency >= cycle || latency > second / 5) return 0;
    return cycle - latency;
}
