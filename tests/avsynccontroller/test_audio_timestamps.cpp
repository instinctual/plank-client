#include "avsynccontroller.h"
#include <cassert>
#include <climits>
#include <iostream>

int main()
{
    using namespace PlankAvSync;
    AudioTimestampObserver observer;
    const VideoClockSample video {100000, 1000, true};
    // The first audio sample is heard after 20ms queued + 10ms device delay.
    auto sample = observer.observe(100030000, 240, 48000, 1000, 20, 10, 0, video, true);
    assert(sample.sourceValid && sample.phaseValid && sample.estimatedLeadUs == 0);
    sample = observer.observe(100035000, 240, 48000, 1005, 20, 10, 0, video, true);
    assert(sample.sourceGapUs == 0 && sample.discontinuities == 0 && sample.estimatedLeadUs == 0);
    sample = observer.observe(100040000, 240, 48000, 1005, 20, 10, 0, video, true);
    assert(sample.estimatedLeadUs == 5000); // ahead
    sample = observer.observe(100045000, 240, 48000, 1020, 20, 10, 0, video, true);
    assert(sample.estimatedLeadUs == -5000); // behind
    // Resampler-held samples are earlier than this source chunk's first frame.
    sample = observer.observe(100050000, 240, 48000, 1020, 20, 10, 1000, video, true);
    assert(sample.estimatedLeadUs == -1000);
    sample = observer.observe(100150000, 240, 48000, 1020, 20, 10, 0, video, true);
    assert(sample.sourceGapUs == 95000 && sample.discontinuities == 1);
    sample = observer.observe(100140000, 240, 48000, 1020, 20, 10, 0, video, true);
    assert(sample.sourceGapUs == -15000 && sample.discontinuities == 2);
    // Linux epochs are independent: never report a fabricated absolute offset.
    assert(!observer.observe(0, 240, 48000, 1000, 20, 10, 0, video, false).phaseValid);
    assert(!observer.observe(0, 240, 48000, 4001, 20, 10, 0, video, true).phaseValid);
    assert(!observer.observe(0, 240, 48000, 999, 20, 10, 0, video, true).phaseValid);
    assert(!observer.observe(0, 240, 48000, 1000, -1, 10, 0, video, true).phaseValid);
    assert(!observer.observe(0, 240, 48000, 1000, 20, -1, 0, video, true).phaseValid);
    assert(!observer.observe(-1, 240, 48000, 1000, 20, 10, 0, video, true).sourceValid);
    assert(!observer.observe(INT64_MAX, 240, 48000, 1000, 20, 10, 0, video, true).sourceValid);
    assert(!observer.observe(1000, 240, 48000, 1000, 20, 10, 0, {INT64_MAX, 1000, true}, true).phaseValid);
    observer.reset();
    sample = observer.observe(100030000, 240, 48000, 10, 20, 10, 0,
                             {99990, UINT32_MAX, true}, true);
    assert(sample.phaseValid && sample.estimatedLeadUs == -1000); // 32-bit monotonic tick wrap
    assert(sample.discontinuities == 0 && sample.sourceGapUs == 0);
    // Two-hour deterministic baseline; media and local epochs are unrelated.
    observer.reset();
    for (int ms = 0; ms < 7200000; ms += 5) {
        const auto audioUs = 100000000LL + ms * 1000LL + 30000;
        const auto render = static_cast<std::uint32_t>(ms + 1000);
        sample = observer.observe(audioUs, 240, 48000, render, 20, 10, 0,
                                  {100000 + ms, render, true}, true);
        assert(sample.phaseValid && sample.estimatedLeadUs == 0 && sample.discontinuities == 0);
    }
    std::cout << "audio_timestamp_observer=pass\n";
}
