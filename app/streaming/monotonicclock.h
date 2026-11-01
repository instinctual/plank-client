#pragma once

#include <chrono>
#include <cstdint>

namespace PlankClock {
// One process-wide epoch for audio/video synchronization, telemetry and
// cancellation deadlines. Wall-clock changes must not alter media timing.
inline std::uint64_t milliseconds()
{
    using Clock = std::chrono::steady_clock;
    static const auto epoch = Clock::now();
    return 1 + std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch).count());
}
}
