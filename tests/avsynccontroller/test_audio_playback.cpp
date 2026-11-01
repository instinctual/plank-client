#include "avsynccontroller.h"
#include "audio/pcmring.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace PlankAvSync;

struct Output
{
    PlankAudio::PcmRing ring {24000, 2};
    AudioPlaybackObserver observer;
    std::uint32_t now = 0;
    bool push(const float* samples, unsigned frames) { return ring.push(samples, frames); }
    unsigned pull(float* samples, unsigned frames)
    {
        const unsigned queued = ring.queued();
        observer.observePull(queued * 8, std::max(0, int(frames * 8) - int(queued * 8)), frames * 8,
                             48000 * 8, now);
        return ring.pop(samples, frames);
    }
};

static AudioTimestampObserver::Observation phase(int us)
{
    AudioTimestampObserver::Observation timing;
    timing.sourceValid = timing.phaseValid = true;
    timing.estimatedLeadUs = us;
    return timing;
}

static void guardTests()
{
    AudioPhaseController controller;
    AudioPlaybackObserver::Observation pull;
    // No output observations: do not accelerate based on queue size alone.
    for (int ms = 0; ms < 2000; ms += 5)
        assert(controller.update(phase(-100000), 35, ms, pull, 5000).correctionPpm == 0);
    for (int ms = 2000; ms < 4000; ms += 20) {
        ++pull.pulls;
        pull.ticks = ms;
        pull.requestUs = 21333;
        pull.headroomUs = 15000;
        controller.update(phase(-100000), 35, ms, pull, 5000);
    }
    assert(controller.correctionPpm() > 0 && !controller.accelerationBlocked());
    // Actual output demand must override a healthy-looking producer queue.
    ++pull.pulls;
    pull.ticks = 4000;
    pull.headroomUs = 0;
    ++pull.shortageRequests;
    assert(controller.update(phase(-100000), 35, 4000, pull, 5000).correctionPpm == 0);
    assert(controller.accelerationBlocked());
    // Repeated producer observations must not count as recovered output pulls.
    pull.headroomUs = 15000;
    for (int ms = 4001; ms < 4020; ++ms)
        assert(controller.update(phase(-100000), 35, ms, pull, 5000).correctionPpm == 0);
    for (int i = 1; i <= 3; ++i) {
        ++pull.pulls;
        pull.ticks = 4000 + i * 20;
        controller.update(phase(-100000), 35, pull.ticks, pull, 5000);
        assert(controller.accelerationBlocked() == (i < 3));
    }
    // Stale device callbacks disable catch-up. Tick arithmetic handles wrap.
    controller.update(phase(-100000), 35, 5000, pull, 5000);
    assert(controller.accelerationBlocked() && controller.correctionPpm() == 0);
    controller.reset();
    pull = {};
    pull.pulls = 1;
    pull.ticks = UINT32_MAX - 10;
    pull.requestUs = 21333;
    pull.headroomUs = 15000;
    controller.update(phase(-100000), 35, 5, pull, 5000);
    assert(controller.correctionPpm() == 0);
    // Skipped observations cannot be counted as consecutive healthy pulls.
    controller.reset();
    pull = {};
    pull.requestUs = 5000;
    pull.headroomUs = 15000;
    for (int i = 1; i <= 4; ++i) {
        pull.pulls += 2;
        pull.ticks = i * 10;
        controller.update(phase(-100000), 35, pull.ticks, pull, 5000);
        assert(controller.accelerationBlocked());
    }
    // Bad input invalidates the snapshot without wrapping byte arithmetic.
    AudioPlaybackObserver observer;
    observer.observePull(4096, 0, 2048, 384000, 100);
    assert(observer.read().headroomUs > 0);
    observer.observePull(-1, 0, 2048, 384000, 105);
    assert(observer.read().headroomUs == -1);
    observer.observePull(INT32_MAX, 0, 1, 1, 110);
    assert(observer.read().headroomUs == INT32_MAX);
}

static void callbackTests()
{
    Output output;
    std::vector<float> data(1024 * 2, 0.1f), samples(1024 * 2);
    assert(output.push(data.data(), 1024));
    assert(output.pull(samples.data(), 1024) == 1024);
    assert(output.ring.queued() == 0);
    assert(output.observer.read().shortageRequests == 0);
    assert(output.push(data.data(), 240));
    assert(output.pull(samples.data(), 1024) == 240);
    auto observation = output.observer.read();
    assert(observation.pulls == 2 && observation.shortageRequests == 1);
    assert(observation.missingInputBytes == (1024 - 240) * 8);
    assert(observation.requestUs == 21333 && observation.headroomUs == 0);
}

// Production PCM ring and FFmpeg compensation; independently scheduled source and
// output clocks. No device, sleeps or network. This does NOT qualify live sync.
static void finiteQueue(int deviceFrames, double skewPpm, int videoDelayMs)
{
    Output output;
    AVChannelLayout channels = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* swr = nullptr;
    assert(swr_alloc_set_opts2(&swr, &channels, AV_SAMPLE_FMT_FLT, 48000,
                             &channels, AV_SAMPLE_FMT_FLT, 48000, 0, nullptr) == 0);
    assert(av_opt_set_int(swr, "flags", SWR_FLAG_RESAMPLE, 0) == 0);
    assert(swr_init(swr) == 0);
    AudioPhaseController controller;
    AudioTimestampObserver observer;
    std::vector<float> input(240 * 2, 0.1f), converted(1024 * 2);
    std::vector<float> device(deviceFrames * 2), initial(1440 * 2, 0.1f);
    assert(output.push(initial.data(), 1440));
    const double periodUs = deviceFrames * 1000000.0 / (48000 * (1 + skewPpm / 1000000));
    double nextPullUs = periodUs;
    std::uint64_t shortReads = 0;
    std::int64_t lastLead = 0;
    std::uint32_t lastCompensation = 0;
    int maximumQueue = 0;
    double settledLeadSum = 0, finalLeadSum = 0;
    int settledSamples = 0, finalSamples = 0;
    constexpr std::int64_t EpochUs = 90000000000LL;
    for (int block = 0; block < 120000; ++block) { // ten minutes of source audio
        // Arrival jitter without packet loss; wall clock never follows resampler
        // output counts. This is the distinction from the clock-only fixtures.
        const double arrivalUs = block * 5000.0 + (block % 7) * 400;
        while (nextPullUs <= arrivalUs) {
            output.now = static_cast<std::uint32_t>(nextPullUs / 1000) + 1000;
            const int got = int(output.pull(device.data(), deviceFrames)) * 8;
            assert(got >= 0);
            if (nextPullUs > 10000000 && got != deviceFrames * 8)
                ++shortReads;
            nextPullUs += periodUs;
        }
        const auto now = static_cast<std::uint32_t>(arrivalUs / 1000) + 1000;
        const int queueMs = output.ring.queued() * 8 * 1000LL / (48000 * 8);
        maximumQueue = std::max(maximumQueue, queueMs);
        const auto timing = observer.observe(EpochUs + block * 5000LL, 240, 48000,
            now, queueMs, deviceFrames * 1000 / 48000, swr_get_delay(swr, 1000000),
            {EpochUs / 1000 + now - 1000 - videoDelayMs, now, true}, true);
        const auto correction = controller.update(timing, queueMs, now,
                                                   output.observer.read(), 5000);
        lastLead = timing.estimatedLeadUs;
        if (block >= 24000 && block < 36000) {
            settledLeadSum += lastLead;
            ++settledSamples;
        }
        if (block >= 108000) {
            finalLeadSum += lastLead;
            ++finalSamples;
        }
        if (correction.updated || now - lastCompensation >= 100) {
            lastCompensation = now;
            assert(swr_set_compensation(swr, static_cast<int>(std::llround(
                -correction.correctionPpm * 48000.0 / 1000000)), 48000) == 0);
        }
        const uint8_t* in[] = {reinterpret_cast<const uint8_t*>(input.data())};
        uint8_t* out[] = {reinterpret_cast<uint8_t*>(converted.data())};
        const int frames = swr_convert(swr, out, 1024, in, 240);
        assert(frames > 0);
        assert(output.push(converted.data(), frames));
    }
    const double settledLead = settledLeadSum / settledSamples;
    const double finalLead = finalLeadSum / finalSamples;
    std::cout << "finite_queue: device_frames=" << deviceFrames << " skew_ppm=" << skewPpm
              << " video_delay_ms=" << videoDelayMs << " short_reads=" << shortReads
              << " max_queue_ms=" << maximumQueue << " last_lead_us=" << lastLead
              << " settled_mean_us=" << settledLead << " final_mean_us=" << finalLead << std::endl;
    assert(shortReads == 0);
    assert(maximumQueue < 75);
    // A bounded endpoint alone is not a drift gate. Compare full settled
    // minute windows and expose the residual offset, including infeasible cases.
    assert(std::abs(finalLead - settledLead) < 10000);
    // For the reachable timing case, correction must still preserve phase.
    // The deliberately infeasible case must not empty audio to fake zero phase.
    if (videoDelayMs == 55)
        assert(std::abs(lastLead) < 20000);
    swr_free(&swr);
}

int main()
{
    guardTests();
    callbackTests();
    finiteQueue(1024, -400, 55);
    finiteQueue(1024, 400, 55);
    finiteQueue(1024, 0, 25);
    finiteQueue(1024, -400, 25);
    finiteQueue(1024, 400, 25);
    finiteQueue(256, 0, 25);
    std::cout << "audio_playback=pass simulated_device=1\n";
}
