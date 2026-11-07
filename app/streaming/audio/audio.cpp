#include <QtGlobal>
#include <QDebug>
#include "../monotonicclock.h"
#include "../session.h"
#include "renderers/renderer.h"

#include <QThread>
#ifdef Q_OS_LINUX
#include "renderers/pipewire.h"
#else
#include "renderers/coreaudio.h"
#endif

IAudioRenderer* Session::createAudioRenderer(const POPUS_MULTISTREAM_CONFIGURATION opusConfig)
{
#ifdef Q_OS_LINUX
    using Renderer = PipeWireAudioRenderer;
#else
    using Renderer = CoreAudioRenderer;
#endif
    auto renderer = std::make_unique<Renderer>(m_Computer->plankAuthentication,
        m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT);
    if (!renderer->prepareForPlayback(opusConfig)) return nullptr;
    return renderer.release();
}

bool Session::initializeAudioRenderer()
{
    int error;

    Q_ASSERT(m_OriginalAudioConfig.channelCount > 0);
    Q_ASSERT(m_AudioRenderer == nullptr);
    Q_ASSERT(m_OpusDecoder == nullptr);

    m_AudioRenderer = createAudioRenderer(&m_OriginalAudioConfig);

    // We may be unable to create an audio renderer right now
    if (m_AudioRenderer == nullptr) {
        return false;
    }

    // Allow the chosen renderer to remap Opus channels as needed to ensure proper output
    m_ActiveAudioConfig = m_OriginalAudioConfig;
    m_AudioRenderer->remapChannels(&m_ActiveAudioConfig);

    // Create the Opus decoder with the renderer's preferred channel mapping
    m_OpusDecoder =
        opus_multistream_decoder_create(m_ActiveAudioConfig.sampleRate,
                                        m_ActiveAudioConfig.channelCount,
                                        m_ActiveAudioConfig.streams,
                                        m_ActiveAudioConfig.coupledStreams,
                                        m_ActiveAudioConfig.mapping,
                                        &error);
    if (m_OpusDecoder == nullptr) {
        delete m_AudioRenderer;
        m_AudioRenderer = nullptr;
        qCritical("Failed to create decoder: %d",
                     error);
        return false;
    }

    qInfo("Audio stream has %d channels",
                m_ActiveAudioConfig.channelCount);
    return true;
}

int Session::getAudioRendererCapabilities(int audioConfiguration)
{
    int caps = 0;

    Q_UNUSED(audioConfiguration);

    // All audio renderers support arbitrary audio duration
    caps |= CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;


    return caps;
}

bool Session::testAudio(int audioConfiguration)
{
    // Build a fake OPUS_MULTISTREAM_CONFIGURATION to give
    // the renderer the channel count and sample rate.
    OPUS_MULTISTREAM_CONFIGURATION opusConfig = {};
    opusConfig.sampleRate = 48000;
    opusConfig.samplesPerFrame = 240;
    opusConfig.channelCount = CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(audioConfiguration);

    IAudioRenderer* audioRenderer = createAudioRenderer(&opusConfig);
    if (audioRenderer == nullptr) {
        return false;
    }

    delete audioRenderer;

    return true;
}

int Session::arInit(int /* audioConfiguration */,
                    const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                    void* /* arContext */, int /* arFlags */)
{
    s_ActiveSession->m_OriginalAudioConfig = *opusConfig;
    s_ActiveSession->initializeAudioRenderer();
    return 0;
}

void Session::arCleanup()
{
    delete s_ActiveSession->m_AudioRenderer;
    s_ActiveSession->m_AudioRenderer = nullptr;

    opus_multistream_decoder_destroy(s_ActiveSession->m_OpusDecoder);
    s_ActiveSession->m_OpusDecoder = nullptr;
}

void Session::arDecodeAndPlaySample(char* sampleData, int sampleLength, int64_t presentationTimeUs)
{
    int samplesDecoded;
    const quint64 mediaFrameIndex = s_ActiveSession->m_AudioMediaFramesReceived++;

    if (s_ActiveSession->m_AudioSampleCount == 0)
        QThread::currentThread()->setPriority(QThread::HighPriority);

    // See if we need to drop this sample
    if (s_ActiveSession->m_DropAudioEndTime != 0) {
        if (PlankClock::milliseconds() >= s_ActiveSession->m_DropAudioEndTime) {
            // Avoid calling PlankClock::milliseconds() now
            s_ActiveSession->m_DropAudioEndTime = 0;

            qInfo("Audio drop window has ended");
        }
        else {
            // We're still in the drop window
            return;
        }
    }

    s_ActiveSession->m_AudioSampleCount++;

    // If audio is muted, don't decode or play the audio
    if (s_ActiveSession->m_AudioMuted) {
        return;
    }

    if (s_ActiveSession->m_AudioRenderer != nullptr) {
        int sampleSize = s_ActiveSession->m_AudioRenderer->getAudioBufferSampleSize();
        int frameSize = sampleSize * s_ActiveSession->m_ActiveAudioConfig.channelCount;
        int desiredBufferSize = frameSize * s_ActiveSession->m_ActiveAudioConfig.samplesPerFrame;
        void* buffer = s_ActiveSession->m_AudioRenderer->getAudioBuffer(&desiredBufferSize);
        if (buffer == nullptr) {
            return;
        }

        if (s_ActiveSession->m_AudioRenderer->getAudioBufferFormat() == IAudioRenderer::AudioFormat::Float32NE) {
            samplesDecoded = opus_multistream_decode_float(s_ActiveSession->m_OpusDecoder,
                                                           (unsigned char*)sampleData,
                                                           sampleLength,
                                                           (float*)buffer,
                                                           desiredBufferSize / frameSize,
                                                           0);
        }
        else {
            samplesDecoded = opus_multistream_decode(s_ActiveSession->m_OpusDecoder,
                                                     (unsigned char*)sampleData,
                                                     sampleLength,
                                                     (short*)buffer,
                                                     desiredBufferSize / frameSize,
                                                     0);
        }

        // Update desiredSize with the number of bytes actually populated by the decoding operation
        if (samplesDecoded > 0) {
            Q_ASSERT(desiredBufferSize >= frameSize * samplesDecoded);
            desiredBufferSize = frameSize * samplesDecoded;
        }
        else {
            desiredBufferSize = 0;
        }

        if (!s_ActiveSession->m_AudioRenderer->submitAudio(desiredBufferSize, presentationTimeUs)) {
            qWarning("Reinitializing audio renderer after failure");

            opus_multistream_decoder_destroy(s_ActiveSession->m_OpusDecoder);
            s_ActiveSession->m_OpusDecoder = nullptr;

            delete s_ActiveSession->m_AudioRenderer;
            s_ActiveSession->m_AudioRenderer = nullptr;
        }
        else if (s_ActiveSession->m_AvSyncTelemetryEnabled) {
            const quint64 now = PlankClock::milliseconds();
            if (s_ActiveSession->m_LastAudioTelemetryTime == 0 ||
                    now - s_ActiveSession->m_LastAudioTelemetryTime >= 1000) {
                const quint64 uncorrectedMediaTimeMs =
                    mediaFrameIndex * s_ActiveSession->m_ActiveAudioConfig.samplesPerFrame * 1000 /
                    s_ActiveSession->m_ActiveAudioConfig.sampleRate;
                const qint64 correctedMediaTimeMs =
                    s_ActiveSession->m_AudioRenderer->getSubmittedAudioMediaTimeMs();
                const quint64 mediaTimeMs = correctedMediaTimeMs >= 0 ?
                    static_cast<quint64>(correctedMediaTimeMs) :
                    uncorrectedMediaTimeMs;
                const int frameDurationMs =
                    s_ActiveSession->m_ActiveAudioConfig.samplesPerFrame * 1000 /
                    s_ActiveSession->m_ActiveAudioConfig.sampleRate;
                qInfo("PLANK A/V audio clock: media=%llu submit=%llu queue=%d device=%d frame=%d correction=%d skipped=%llu raw=%llu catchup=%d",
                            static_cast<unsigned long long>(mediaTimeMs),
                            static_cast<unsigned long long>(now),
                            s_ActiveSession->m_AudioRenderer->getQueuedAudioDurationMs(),
                            s_ActiveSession->m_AudioRenderer->getDeviceBufferDurationMs(),
                            frameDurationMs,
                            s_ActiveSession->m_AudioRenderer->getAudioClockCorrectionPpm(),
                            static_cast<unsigned long long>(
                                s_ActiveSession->m_AudioRenderer->getSkippedAudioBlockCount()),
                            static_cast<unsigned long long>(uncorrectedMediaTimeMs),
                            s_ActiveSession->m_AudioRenderer->getAudioBacklogCorrectionPpm());
                s_ActiveSession->m_LastAudioTelemetryTime = now;
            }
        }
    }

    // Only try to recreate the audio renderer every 200 samples (1 second)
    // to avoid thrashing if the audio device is unavailable. It is
    // safe to reinitialize here because we can't be torn down while
    // the audio decoder/playback thread is still alive.
    if (s_ActiveSession->m_AudioRenderer == nullptr && (s_ActiveSession->m_AudioSampleCount % 200) == 0) {
        // Since we're doing this inline and audio initialization takes time, we need
        // to drop samples to account for the time we've spent blocking audio rendering
        // so we return to real-time playback and don't accumulate latency.
        quint64 audioReinitStartTime = PlankClock::milliseconds();
        if (s_ActiveSession->initializeAudioRenderer()) {
            quint64 audioReinitStopTime = PlankClock::milliseconds();

            s_ActiveSession->m_DropAudioEndTime = audioReinitStopTime + (audioReinitStopTime - audioReinitStartTime);
            qInfo("Audio reinitialization took %llu ms - starting drop window",
                        static_cast<unsigned long long>(
                            audioReinitStopTime - audioReinitStartTime));
        }
    }
}
