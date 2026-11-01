// Production realtime callback and snapshot tests. Never opens an audio device,
// installs a property listener or requests microphone permission on the runner.
#include <QtTest>
#include "streaming/audio/renderers/coreaudio.h"
#include <thread>

class TestCoreAudio : public QObject {
    Q_OBJECT
    void configure(CoreAudioRenderer& renderer) {
        renderer.m_ChannelCount = 2;
        renderer.m_SampleRate = 48000;
        renderer.m_BytesPerSampleFrame = 8;
        renderer.m_BytesPerSecond = 384000;
        renderer.m_Ring = std::make_unique<PlankAudio::PcmRing>(2048, 2);
    }
private slots:
    void callbackCopiesPcmAndReportsRealShortage() {
        CoreAudioRenderer renderer;
        configure(renderer);
        float input[256 * 2], output[512 * 2];
        std::fill(std::begin(input), std::end(input), 0.125f);
        std::fill(std::begin(output), std::end(output), 1.0f);
        QVERIFY(renderer.m_Ring->push(input, 256));
        AudioBufferList list {1, {{2, sizeof(output), output}}};
        AudioUnitRenderActionFlags flags = 0;
        QCOMPARE(CoreAudioRenderer::render(&renderer, &flags, nullptr, 0, 512, &list), noErr);
        for (unsigned i = 0; i < 1024; ++i) QCOMPARE(output[i], i < 512 ? 0.125f : 0.0f);
        const auto observation = renderer.m_PlaybackSnapshot.read();
        QCOMPARE(observation.pulls, std::uint64_t(1));
        QCOMPARE(observation.shortageRequests, std::uint64_t(1));
        QCOMPARE(observation.missingInputBytes, std::uint64_t(256 * 8));
        QVERIFY(renderer.m_LastPull.load() > 0);
        flags = 0;
        QCOMPARE(CoreAudioRenderer::render(&renderer, &flags, nullptr, 0, 512, &list), noErr);
        QVERIFY(flags & kAudioUnitRenderAction_OutputIsSilence);
    }
    void fulfilledPullDoesNotReportStarvation() {
        CoreAudioRenderer renderer;
        configure(renderer);
        float samples[256 * 2] {};
        QVERIFY(renderer.m_Ring->push(samples, 256));
        AudioBufferList list {1, {{2, sizeof(samples), samples}}};
        QCOMPARE(CoreAudioRenderer::render(&renderer, nullptr, nullptr, 0, 256, &list), noErr);
        QCOMPARE(renderer.m_PlaybackSnapshot.read().shortageRequests, std::uint64_t(0));
        QCOMPARE(renderer.m_Ring->queued(), 0u);
    }
    void invalidBufferFailsWithoutOverrun() {
        CoreAudioRenderer renderer;
        configure(renderer);
        float samples[512]; std::fill(std::begin(samples), std::end(samples), 1.0f);
        AudioBufferList list {1, {{2, 4, samples}}};
        QVERIFY(CoreAudioRenderer::render(&renderer, nullptr, nullptr, 0, 256, &list) != noErr);
        QVERIFY(renderer.m_Failed.load());
        QCOMPARE(samples[0], 0.0f);
        QCOMPARE(samples[1], 1.0f);
    }
    void snapshotNeverMixesCallbackGenerations() {
        CoreAudioPlaybackSnapshot snapshot;
        std::atomic_bool done {false};
        std::thread writer([&] {
            for (unsigned i = 1; i <= 100000; ++i)
                snapshot.publish({i, i, i, i, int(i), int(i)});
            done.store(true);
        });
        bool consistent = true;
        do {
            auto value = snapshot.read();
            if (value.pulls)
                consistent &= value.pulls == value.shortageRequests && value.pulls == value.missingInputBytes &&
                    value.pulls == value.ticks && value.pulls == unsigned(value.requestUs) &&
                    value.pulls == unsigned(value.headroomUs);
        } while (!done.load());
        writer.join();
        QVERIFY(consistent);
        QCOMPARE(snapshot.read().pulls, std::uint64_t(100000));
    }
};
QTEST_GUILESS_MAIN(TestCoreAudio)
#include "test_macnativeaudio.moc"
