#pragma once
#include "../avsynccontroller.h"
#include <array>
#include <atomic>

// Single realtime writer, worker reader. Every field is atomic; a sequence
// counter alone around an ordinary struct would still create a C++ data race.
class CoreAudioPlaybackSnapshot {
public:
    using Observation = PlankAvSync::AudioPlaybackObserver::Observation;
    void publish(const Observation& o) {
        m_Sequence.fetch_add(1);
        const std::array<std::int64_t, 6> values {std::int64_t(o.pulls), std::int64_t(o.shortageRequests),
            std::int64_t(o.missingInputBytes), o.ticks, o.requestUs, o.headroomUs};
        for (unsigned i = 0; i < values.size(); ++i) m_Values[i].store(values[i]);
        m_Sequence.fetch_add(1);
    }
    Observation read() const {
        for (int retry = 0; retry < 4; ++retry) {
            const auto before = m_Sequence.load();
            if (before & 1) continue;
            std::array<std::int64_t, 6> values;
            for (unsigned i = 0; i < values.size(); ++i) values[i] = m_Values[i].load();
            if (before == m_Sequence.load()) return {std::uint64_t(values[0]), std::uint64_t(values[1]),
                std::uint64_t(values[2]), std::uint32_t(values[3]), int(values[4]), int(values[5])};
        }
        return {};
    }
private:
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> m_Sequence {0};
    std::array<std::atomic<std::int64_t>, 6> m_Values {};
};
