#include "streaming/audio/renderers/pipewire.h"
#include "streaming/audio/renderers/pipewirebuffer.h"
#include "streaming/avsynccontroller.h"
#include <QtTest>
#include <SDL3/SDL.h>
#include <dbus/dbus.h>
#include <thread>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
using namespace PlankPipeWire;

class PipeWireAudioTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // The RTKit module loads libdbus during each PipeWire context's
        // construction. Without a process-lifetime owner, dlclose loses its
        // global caches/locks between our isolated repeated-lifecycle cases.
        // Keep it linked and use libdbus's documented leak-checker cleanup.
        QVERIFY(dbus_threads_init_default());
    }
    void cleanupTestCase()
    {
        // All renderers and their threads have been destroyed here. Never do
        // this from the renderer: other components of a real GUI use D-Bus.
        // This frees only library globals, not outstanding application objects.
        // https://dbus.freedesktop.org/doc/api/html/group__DBusMemory.html
        dbus_shutdown();
    }
    void boundedRingPreservesOrder()
    {
        PcmRing ring(8, 2);
        const float samples[] {1,11,2,12,3,13,4,14,5,15,6,16};
        float out[16] {};
        QVERIFY(ring.push(samples, 6));
        QVERIFY(!ring.push(samples, 3));
        QCOMPARE(ring.pop(out, 4), 4U);
        QCOMPARE(out[6], 4.0f);
        QVERIFY(ring.push(samples, 6)); // wrap in both planes
        QCOMPARE(ring.pop(out, 8), 8U);
        QCOMPARE(out[0], 5.0f); QCOMPARE(out[2], 6.0f);
        for (unsigned n = 0; n < 12; ++n) QCOMPARE(out[n+4], samples[n]);
        QCOMPARE(ring.queued(), 0U);
        QCOMPARE(ring.pop(out, 8), 0U);
    }
    void concurrentRingNeverEvicts()
    {
        PcmRing ring(1024, 2);
        constexpr unsigned count = 1000000;
        std::atomic<bool> correct {true};
        std::thread producer([&] {
            for (unsigned n = 0; n < count; ++n) {
                const float pair[] {float(n), -float(n)};
                while (!ring.push(pair, 1)) std::this_thread::yield();
            }
        });
        std::thread consumer([&] {
            float out[64];
            for (unsigned n = 0; n < count;) {
                const unsigned read = ring.pop(out, std::min(32U, count-n));
                for (unsigned f = 0; f < read; ++f, ++n)
                    if (out[2*f] != float(n) || out[2*f+1] != -float(n)) correct.store(false);
                if (!read) std::this_thread::yield();
            }
        });
        producer.join(); consumer.join();
        QVERIFY(correct.load()); QCOMPARE(ring.queued(), 0U);
    }
    void coherentClockSnapshot()
    {
        ClockSnapshot snapshot;
        std::atomic<bool> done {false}, valid {true};
        std::thread writer([&] {
            for (unsigned n = 1; n < 100000; ++n) {
                OutputClock c; c.observedNs = n; c.tailNs = n+1; c.pulls = n*2;
                snapshot.publish(c);
            }
            done.store(true);
        });
        do {
            const auto c = snapshot.get();
            if (c.observedNs && (c.tailNs != c.observedNs+1 || c.pulls != c.observedNs*2)) valid.store(false);
        } while (!done.load());
        writer.join(); QVERIFY(valid.load());
    }
    void clockAccountsForGraphAndRingOnce()
    {
        const auto tail = presentationTailNs(8000000000LL, 480, 1, 48000, 512, 96, 48000, 8002000000ULL);
        QCOMPARE(tail, 8022666666ULL);
        OutputClock c; c.observedNs = 8002000000ULL; c.tailNs = tail; c.consumed = 1000; c.priming = false;
        QCOMPARE(queueDelayUs(c, 1480, 8002000000ULL, 48000), 30666LL);
        QCOMPARE(queueDelayUs(c, 1480, 8007000000ULL, 48000), 25666LL);
        QCOMPARE(queueDelayUs(c, 999, 8002000000ULL, 48000), -1LL);
        QCOMPARE(queueDelayUs(c, 1480, 8202000000ULL, 48000), -1LL);
        QCOMPARE(presentationTailNs(0, 0, 1, 48000, 0, 0, 48000, 1), 0ULL);
        QCOMPARE(presentationTailNs(8000000000LL, INT64_MAX, 1, 1, 0, 0, 48000, 8000000000ULL), 0ULL);
        QCOMPARE(presentationTailNs(8000000000LL, 0, 1, 48000, UINT64_MAX, 0, 48000, 8000000000ULL), 0ULL);
        c.priming = true; QCOMPARE(queueDelayUs(c, 1480, 8002000000ULL, 48000), -1LL);
    }
    void startStallAndRecoveryAreBounded()
    {
        PcmRing ring(9600, 2); Playback player(ring, 48000, 2);
        std::vector<float> in(3200, 1.0f), out(2048);
        QCOMPARE(player.render(out.data(), 1024), 0U);
        QCOMPARE(player.clock().underruns, 0ULL);
        QVERIFY(ring.push(in.data(), 1600));
        QCOMPARE(player.render(out.data(), 1024), 1024U);
        QVERIFY(out[0] > 0.0f && out[0] < 1.0f); QCOMPARE(out[500], 1.0f);
        QCOMPARE(player.render(out.data(), 1024), 576U);
        QCOMPARE(player.clock().underruns, 1ULL);
        QCOMPARE(player.clock().missingFrames, 448ULL);
        QCOMPARE(out[1150], 0.0f); QCOMPARE(out[2047], 0.0f);
        for (int n = 0; n < 10; ++n) QCOMPARE(player.render(out.data(), 1024), 0U);
        QCOMPARE(player.clock().underruns, 1ULL);
        QCOMPARE(player.clock().missingFrames, 10688ULL);
        QVERIFY(ring.push(in.data(), 1600));
        QCOMPARE(player.render(out.data(), 1024), 1024U);
        QVERIFY(!player.clock().priming);
    }
    void guardKeepsLowWaterAcrossProducerBursts()
    {
        HeadroomGuard guard;
        int ppm = 0;
        for (unsigned now = 1; now < 2000; now += 5) ppm = guard.apply(4000, 25000, now, true);
        QVERIFY(ppm > 0);
        QVERIFY(guard.apply(4000, 2000, 2000, true) <= 0);
        for (unsigned now = 2005; now < 2900; now += 5)
            QVERIFY(guard.apply(4000, now%10 ? -1 : 30000, now, true) <= 0);
        QCOMPARE(guard.apply(4000, -1, 2905, false), 0);
    }
    void batchedSourceAndDriftingDevice_data()
    {
        QTest::addColumn<int>("quantum"); QTest::addColumn<int>("drift");
        for (int quantum : {128,512,1024}) for (int drift : {-400,0,400})
            QTest::newRow(qPrintable(QString("quantum%1-drift%2").arg(quantum).arg(drift))) << quantum << drift;
    }
    void batchedSourceAndDriftingDevice()
    {
        QFETCH(int, quantum); QFETCH(int, drift);
        // Actual FFmpeg compensator, 512-frame Host capture bursts split into
        // 240-frame network blocks, up to 3.5ms arrival jitter, device +/-400ppm.
        // A persistent +4000ppm phase request reproduces the live starvation
        // pressure; queue safety must not repeatedly speed into a short read.
        AVChannelLayout layout = AV_CHANNEL_LAYOUT_STEREO;
        SwrContext* swr = nullptr;
        QVERIFY(swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLT, 48000,
            &layout, AV_SAMPLE_FMT_FLT, 48000, 0, nullptr) >= 0);
        struct FreeSwr { SwrContext*& p; ~FreeSwr() { swr_free(&p); } } cleanup {swr};
        QVERIFY(av_opt_set_int(swr, "flags", SWR_FLAG_RESAMPLE, 0) >= 0); QVERIFY(swr_init(swr) >= 0);
        PcmRing ring(9600, 2); Playback player(ring, 48000, 2); HeadroomGuard guard;
        std::vector<float> in(480, 0.2f), out(4096), resampled(4096);
        const int jitter[] {0, 1500, 3500, 500};
        double pullUs = quantum * 1000000.0 / (48000.0 * (1.0 + drift / 1000000.0));
        const double pullStep = pullUs;
        unsigned batch = 0, pending = 0, largestQueue = 0;
        int minimum = INT32_MAX, lastPpm = INT32_MIN;
        std::uint64_t lastCompensation = 0;
        for (std::uint64_t now = 0; now < 90000000; now += 100) {
            if (now >= (batch+1) * (512000000.0 / 48000) + jitter[batch % 4]) {
                ++batch; pending += 512;
                while (pending >= 240) {
                    const auto c = player.clock();
                    const int ppm = guard.apply(4000, minimum == INT32_MAX ? -1 : minimum,
                        now/1000, c.pulls && !c.priming);
                    minimum = INT32_MAX;
                    if (ppm != lastPpm || now-lastCompensation >= 100000) {
                        QVERIFY(swr_set_compensation(swr, std::llround(-double(ppm)*48000/1000000), 48000) >= 0);
                        lastPpm = ppm; lastCompensation = now;
                    }
                    const std::uint8_t* inputs[] {reinterpret_cast<const std::uint8_t*>(in.data())};
                    std::uint8_t* outputs[] {reinterpret_cast<std::uint8_t*>(resampled.data())};
                    const int frames = swr_convert(swr, outputs, 2048, inputs, 240);
                    QVERIFY(frames >= 0); QVERIFY(ring.push(resampled.data(), frames));
                    largestQueue = std::max(largestQueue, ring.queued()); pending -= 240;
                }
            }
            if (now >= pullUs) {
                player.render(out.data(), quantum);
                const auto c = player.clock();
                minimum = std::min(minimum, c.priming ? 0 : int(c.headroom * 1000000LL / 48000));
                pullUs += pullStep;
            }
        }
        QCOMPARE(player.clock().underruns, 0ULL);
        QVERIFY2(largestQueue < 4800, qPrintable(QString("queue grew to %1 frames").arg(largestQueue)));
    }
    void rejectedFormatAndUnconnectedTeardown()
    {
        PipeWireAudioRenderer output(true, true);
        OPUS_MULTISTREAM_CONFIGURATION config {};
        config.sampleRate = 44100; config.channelCount = 2; config.samplesPerFrame = 240;
        QVERIFY(!output.prepareForPlayback(&config));
        config.sampleRate = 48000; config.channelCount = 100;
        QVERIFY(!output.prepareForPlayback(&config));
        QVERIFY(!output.prepareForPlayback(nullptr));
        QCOMPARE(SDL_WasInit(SDL_INIT_AUDIO), 0U);
    }
    void realPipeWirePlayback()
    {
        if (qEnvironmentVariable("PLANK_PIPEWIRE_TEST") != "1") QSKIP("Isolated PipeWire daemon gate runs separately");
        OPUS_MULTISTREAM_CONFIGURATION config {};
        config.sampleRate = 48000; config.channelCount = 2; config.samplesPerFrame = 240;
        // Repeated construction/destruction while the RT consumer is active.
        // Both Host timing modes run against the identical direct backend.
        for (int run = 0; run < 4; ++run) {
            PipeWireAudioRenderer output(true, run%2);
            QVERIFY(output.prepareForPlayback(&config));
            QElapsedTimer elapsed; elapsed.start();
            for (int block = 0; block < 400; ++block) {
                while (elapsed.nsecsElapsed() < block * 5000000LL) QTest::qSleep(1);
                const auto now = SDL_GetTicks();
                PlankAvSync::publishVideoClock(block * 5, now);
                int bytes = 240 * 2 * sizeof(float);
                auto* samples = static_cast<float*>(output.getAudioBuffer(&bytes));
                QCOMPARE(bytes, 1920);
                for (int frame = 0; frame < 240; ++frame) {
                    const float value = std::sin((block*240+frame) * 440.0 * 6.28318530718 / 48000) * 0.05;
                    samples[frame*2] = samples[frame*2+1] = value;
                }
                QVERIFY(output.submitAudio(bytes, block * 5000LL));
                QVERIFY(output.getQueuedAudioDurationMs() <= 100);
            }
            const auto stats = output.diagnostics();
            QVERIFY(stats.pulls > 20); QVERIFY(stats.quantumFrames > 0); QVERIFY(!stats.priming);
            QVERIFY(output.getDeviceBufferDurationMs() >= 0);
            QCOMPARE(SDL_WasInit(SDL_INIT_AUDIO), 0U); // no hidden SDL output
            // Hosted scheduling is not acoustic qualification. A severe stall
            // still fails, but do not equate a virtual sink with real HDMI.
            QVERIFY(stats.underruns <= 2);
        }
    }
    void unavailableServerFailsPromptly()
    {
        if (qEnvironmentVariable("PLANK_PIPEWIRE_TEST") != "1") QSKIP("Isolated daemon test only");
        const QByteArray previous = qgetenv("PIPEWIRE_REMOTE");
        qputenv("PIPEWIRE_REMOTE", "plank-nonexistent-test-server");
        OPUS_MULTISTREAM_CONFIGURATION config {};
        config.sampleRate = 48000; config.channelCount = 2; config.samplesPerFrame = 240;
        QElapsedTimer elapsed; elapsed.start();
        bool prepared;
        {
            PipeWireAudioRenderer output(true, true);
            prepared = output.prepareForPlayback(&config);
        }
        if (previous.isEmpty()) qunsetenv("PIPEWIRE_REMOTE");
        else qputenv("PIPEWIRE_REMOTE", previous);
        QVERIFY(!prepared);
        QVERIFY(elapsed.elapsed() < 2500);
        QCOMPARE(SDL_WasInit(SDL_INIT_AUDIO), 0U);
    }
};
QTEST_GUILESS_MAIN(PipeWireAudioTest)
#include "test_pipewireaudio.moc"
