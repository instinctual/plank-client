#pragma once

#include "renderer.h"
#include <memory>

// Native playback is deliberately limited to PLANK Client Qt. The ordinary
// Linux/macOS Client's SDL renderer and Host capture are not changed.
class PipeWireAudioRenderer : public IAudioRenderer
{
public:
    explicit PipeWireAudioRenderer(bool enableAvSyncCorrection, bool commonAudioVideoEpoch);
    ~PipeWireAudioRenderer() override;
    bool prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION*) override;
    void* getAudioBuffer(int* size) override;
    bool submitAudio(int bytesWritten, qint64 sourceTimeUs) override;
    int getCapabilities() override;
    AudioFormat getAudioBufferFormat() override { return AudioFormat::Float32NE; }
    int getQueuedAudioDurationMs() override;
    int getDeviceBufferDurationMs() override;
    qint64 getSubmittedAudioMediaTimeMs() override;
    int getAudioClockCorrectionPpm() override;
    int getAudioBacklogCorrectionPpm() override;

    struct Diagnostics {
        quint64 pulls = 0, underruns = 0, missingFrames = 0;
        int queueMs = 0, quantumFrames = 0;
        bool priming = true;
    };
    Diagnostics diagnostics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
