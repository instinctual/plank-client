#pragma once

#include "renderer.h"
#include "streaming/avsynccontroller.h"
#include "../coreaudiodevice.h"
#include "../pcmring.h"
#include "../coreaudioplaybackstate.h"
#include <vector>

struct SwrContext;

class CoreAudioRenderer : public IAudioRenderer
{
public:
    explicit CoreAudioRenderer(bool enableAvSyncCorrection = false, bool commonAudioVideoEpoch = false);

    ~CoreAudioRenderer() override;

    bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig) override;

    void* getAudioBuffer(int* size) override;

    bool submitAudio(int bytesWritten, qint64 sourceTimeUs) override;

    int getCapabilities() override;

    int getQueuedAudioDurationMs() override;

    int getDeviceBufferDurationMs() override;

    qint64 getSubmittedAudioMediaTimeMs() override;

    int getAudioClockCorrectionPpm() override;

    AudioFormat getAudioBufferFormat() override;

private:
    friend class TestCoreAudio;
    static OSStatus render(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);
    int queueLimitMs() const;
    AudioUnit m_Unit = nullptr;
    PlankCoreAudio::DeviceWatch m_Device;
    std::unique_ptr<PlankAudio::PcmRing> m_Ring;
    std::atomic_bool m_Failed {false};
    std::atomic<std::uint64_t> m_LastPull {0};
    CoreAudioPlaybackSnapshot m_PlaybackSnapshot;
    void* m_AudioBuffer;
    int m_FrameSize;
    int m_FrameDurationMs;
    int m_BytesPerSampleFrame;
    int m_BytesPerSecond;
    int m_DeviceBufferDurationMs;
    int m_SampleRate;
    int m_ChannelCount;
    bool m_EnableAvSyncCorrection;
    quint64 m_RawAudioFrames;
    quint64 m_SubmittedAudioFrames;
    qint64 m_LastSubmittedAudioMediaTimeMs;
    PlankAvSync::AudioRateController m_AudioRateController;
    PlankAvSync::AudioPhaseController m_AudioPhaseController;
    bool m_CommonAudioVideoEpoch;
    quint64 m_LastTimestampTelemetry = 0;
    quint64 m_LastCorrectionTelemetry = 0;
    quint64 m_LastCompensationUpdate = 0;
    PlankAvSync::AudioTimestampObserver m_AudioTimestampObserver;
    PlankAvSync::AudioPlaybackObserver m_PlaybackObserver;

    SwrContext* m_SwrContext;
    std::vector<float> m_CorrectedAudioBuffer;
};
