#include "coreaudio.h"
#include "../../monotonicclock.h"
#include <QDebug>
#include <QThread>
#include <cstdlib>
#include <cstring>

#include <Limelight.h>
#include <cmath>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

CoreAudioRenderer::CoreAudioRenderer(bool enableAvSyncCorrection, bool commonAudioVideoEpoch)
    : m_AudioBuffer(nullptr),
      m_FrameSize(0),
      m_FrameDurationMs(0),
      m_BytesPerSampleFrame(0),
      m_BytesPerSecond(0),
      m_DeviceBufferDurationMs(0),
      m_SampleRate(0),
      m_ChannelCount(0),
      m_EnableAvSyncCorrection(enableAvSyncCorrection),
      m_RawAudioFrames(0),
      m_SubmittedAudioFrames(0),
      m_LastSubmittedAudioMediaTimeMs(-1),
      m_CommonAudioVideoEpoch(commonAudioVideoEpoch),
      m_SwrContext(nullptr)
{
}

bool CoreAudioRenderer::prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* opusConfig)
{
    if (!opusConfig || opusConfig->sampleRate != 48000 || opusConfig->samplesPerFrame <= 0 ||
        opusConfig->samplesPerFrame > 2880 ||
        (opusConfig->channelCount != 1 && opusConfig->channelCount != 2 &&
         opusConfig->channelCount != 6 && opusConfig->channelCount != 8)) return false;
    const auto want = PlankCoreAudio::floatFormat(opusConfig->sampleRate, opusConfig->channelCount);
    m_FrameDurationMs = opusConfig->samplesPerFrame * 1000 / opusConfig->sampleRate;
    m_FrameSize = opusConfig->samplesPerFrame * opusConfig->channelCount * sizeof(float);
    m_BytesPerSecond = opusConfig->sampleRate * opusConfig->channelCount * sizeof(float);
    m_BytesPerSampleFrame = opusConfig->channelCount * sizeof(float);
    m_SampleRate = opusConfig->sampleRate;
    m_ChannelCount = opusConfig->channelCount;
    AudioDeviceID device = kAudioObjectUnknown;
    if (!m_Device.start(false, device) || !PlankCoreAudio::createUnit(kAudioUnitSubType_DefaultOutput, m_Unit)) return false;
    if (AudioUnitSetProperty(m_Unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &want, sizeof(want)) != noErr) return false;
    AudioChannelLayout layout {};
    switch (m_ChannelCount) {
    case 1: layout.mChannelLayoutTag = kAudioChannelLayoutTag_Mono; break;
    case 2: layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo; break;
    case 6: layout.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A; break;
    case 8: layout.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_7_1_C; break;
    }
    if (AudioUnitSetProperty(m_Unit, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0,
                             &layout, sizeof(layout)) != noErr) return false;
    m_Ring = std::make_unique<PlankAudio::PcmRing>(m_SampleRate / 2, m_ChannelCount);
    AURenderCallbackStruct callback {render, this};
    if (AudioUnitSetProperty(m_Unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                             &callback, sizeof(callback)) != noErr || AudioUnitInitialize(m_Unit) != noErr) return false;
    m_DeviceBufferDurationMs = PlankCoreAudio::outputLatencyMs(device, m_Unit);
    if (m_DeviceBufferDurationMs < 0) return false;

    if (m_EnableAvSyncCorrection) {
        AVChannelLayout channelLayout;
        av_channel_layout_default(&channelLayout, m_ChannelCount);
        const int allocationResult = swr_alloc_set_opts2(
            &m_SwrContext,
            &channelLayout,
            AV_SAMPLE_FMT_FLT,
            m_SampleRate,
            &channelLayout,
            AV_SAMPLE_FMT_FLT,
            opusConfig->sampleRate,
            0,
            nullptr);
        if (m_SwrContext != nullptr) {
            // Keep the resampler active from the first block. Otherwise the
            // first compensation request would reinitialize it mid-stream.
            av_opt_set_int(m_SwrContext, "flags", SWR_FLAG_RESAMPLE, 0);
        }
        av_channel_layout_uninit(&channelLayout);
        if (allocationResult < 0 || m_SwrContext == nullptr ||
                swr_init(m_SwrContext) < 0) {
            qWarning("Unable to initialize PLANK A/V audio correction");
            swr_free(&m_SwrContext);
            m_EnableAvSyncCorrection = false;
        }
        else {
            qInfo("PLANK audio correction enabled: mode=%s limit=%d ppm",
                        m_CommonAudioVideoEpoch ? "source-phase" : "relative-rate",
                        m_CommonAudioVideoEpoch ?
                            PlankAvSync::AudioPhaseController::MaximumCorrectionPpm :
                            PlankAvSync::AudioRateController::MaximumCorrectionPpm);
        }
    }

    m_AudioBuffer = std::malloc(m_FrameSize);
    if (m_AudioBuffer == nullptr) {
        qCritical("Failed to allocate audio buffer");
        return false;
    }

    qInfo("Decoded audio block: %u samples (%u bytes)",
                opusConfig->samplesPerFrame,
                m_FrameSize);

    qInfo("Core Audio playback: OS-selected output, %d channels, %d Hz, device latency %d ms",
          m_ChannelCount, m_SampleRate, m_DeviceBufferDurationMs);
    qInfo("PLANK A/V source timing begin: common=%d observation_only=%d",
                m_CommonAudioVideoEpoch,
                !(m_EnableAvSyncCorrection && m_CommonAudioVideoEpoch));

    m_LastPull.store(PlankClock::milliseconds());
    if (AudioOutputUnitStart(m_Unit) != noErr || !m_Device.valid()) return false;

    return true;
}

CoreAudioRenderer::~CoreAudioRenderer()
{
    // Stop/uninitialize before releasing anything reachable by the RT callback.
    PlankCoreAudio::closeUnit(m_Unit);
    std::free(m_AudioBuffer);
    swr_free(&m_SwrContext);
}

OSStatus CoreAudioRenderer::render(void* context, AudioUnitRenderActionFlags* flags,
                                    const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* buffers)
{
    auto& self = *static_cast<CoreAudioRenderer*>(context);
    if (!buffers) { self.m_Failed.store(true); return kAudio_ParamError; }
    if (buffers->mNumberBuffers != 1 || !buffers->mBuffers[0].mData || !frames ||
        frames > unsigned(self.m_SampleRate / 5) ||
        buffers->mBuffers[0].mNumberChannels != unsigned(self.m_ChannelCount) ||
        buffers->mBuffers[0].mDataByteSize < frames * self.m_BytesPerSampleFrame) {
        for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
            if (buffers->mBuffers[i].mData) std::memset(buffers->mBuffers[i].mData, 0, buffers->mBuffers[i].mDataByteSize);
        self.m_Failed.store(true);
        return kAudio_ParamError;
    }
    auto* output = static_cast<float*>(buffers->mBuffers[0].mData);
    const auto now = PlankClock::milliseconds();
    self.m_LastPull.store(now);
    const unsigned queued = self.m_Ring->queued();
    const int bytes = frames * self.m_BytesPerSampleFrame;
    self.m_PlaybackObserver.observePull(queued * self.m_BytesPerSampleFrame,
        std::max(0, bytes - int(queued * self.m_BytesPerSampleFrame)), bytes,
        self.m_BytesPerSecond, std::uint32_t(now));
    self.m_PlaybackSnapshot.publish(self.m_PlaybackObserver.read());
    const unsigned read = self.m_Failed.load() ? 0 : self.m_Ring->pop(output, frames);
    std::fill(output + read * self.m_ChannelCount, output + frames * self.m_ChannelCount, 0.0f);
    if (!read && flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
    buffers->mBuffers[0].mDataByteSize = bytes;
    return noErr;
}

void* CoreAudioRenderer::getAudioBuffer(int* size)
{
    if (size) *size = m_FrameSize;
    return m_AudioBuffer;
}

bool CoreAudioRenderer::submitAudio(int bytesWritten, qint64 sourceTimeUs)
{
    if (bytesWritten == 0) {
        // Nothing to do
        return true;
    }

    const int inputFrames = m_BytesPerSampleFrame == 0 ?
                                0 : bytesWritten / m_BytesPerSampleFrame;

    if (bytesWritten < 0 || bytesWritten > m_FrameSize || !inputFrames || bytesWritten % m_BytesPerSampleFrame) return false;
    // Backpressure is bounded; a stopped output must cause device recovery,
    // never an unbounded queue or a join that hangs during disconnect.
    for (int i = 0; getQueuedAudioDurationMs() > 50 && i < 100; ++i) {
        if (m_Failed.load() || !m_Device.valid()) return false;
        QThread::msleep(1);
    }
    if (m_Failed.load() || !m_Device.valid() ||
        PlankClock::milliseconds() - m_LastPull.load() > 500 || getQueuedAudioDurationMs() > 50) return false;

    const void* queuedBuffer = m_AudioBuffer;
    int queuedBytes = bytesWritten;
    qint64 resamplerDelayUs = 0;

    if (m_SwrContext != nullptr)
        resamplerDelayUs = swr_get_delay(m_SwrContext, 1000000);
    // One observation supplies both control and diagnostics at the same
    // pre-conversion/pre-enqueue boundary; do not count source gaps twice.
    const quint64 observedAt = PlankClock::milliseconds();
    const int queuedMs = getQueuedAudioDurationMs();
    const auto playback = m_PlaybackSnapshot.read();
    const auto videoClock = PlankAvSync::readVideoClock();
    const auto timing = m_AudioTimestampObserver.observe(
        sourceTimeUs, inputFrames, m_SampleRate, static_cast<quint32>(observedAt),
        queuedMs, m_DeviceBufferDurationMs, resamplerDelayUs,
        videoClock, m_CommonAudioVideoEpoch);

    if (m_EnableAvSyncCorrection && m_SwrContext != nullptr && inputFrames > 0) {
        int appliedCorrectionPpm;
        bool correctionUpdated;
        if (m_CommonAudioVideoEpoch) {
            const auto correction = m_AudioPhaseController.update(
                timing, queuedMs, static_cast<quint32>(observedAt), playback,
                inputFrames * 1000000LL / m_SampleRate);
            appliedCorrectionPpm = correction.correctionPpm;
            correctionUpdated = correction.updated;
        }
        else {
            // Linux Host clocks have independent epochs. Preserve its existing
            // relative-rate policy; never invent an absolute phase or combine
            // both controllers on the same stream.
            const auto correction = m_AudioRateController.update(
                m_RawAudioFrames, m_SubmittedAudioFrames, m_SampleRate,
                static_cast<quint32>(observedAt), videoClock);
            appliedCorrectionPpm = correction.correctionPpm;
            correctionUpdated = correction.updated;
        }
        // Refresh the one-second compensation before it expires, including
        // the unchanged Linux relative-rate policy between its 1Hz updates.
        if (correctionUpdated || observedAt - m_LastCompensationUpdate >= 100) {
            m_LastCompensationUpdate = observedAt;
            constexpr int CompensationSeconds = 1;
            const int compensationDistance = m_SampleRate * CompensationSeconds;
            const int sampleDelta = static_cast<int>(std::llround(
                -static_cast<double>(appliedCorrectionPpm) *
                compensationDistance / 1000000.0));
            if (swr_set_compensation(m_SwrContext,
                                     sampleDelta,
                                     compensationDistance) < 0) {
                qWarning("Unable to update PLANK audio correction");
            }
            else if (observedAt - m_LastCorrectionTelemetry >= 10000) {
                m_LastCorrectionTelemetry = observedAt;
                qInfo("PLANK A/V audio correction: mode=%s applied=%d delta=%d distance=%d",
                            m_CommonAudioVideoEpoch ? "source-phase" : "relative-rate",
                            appliedCorrectionPpm,
                            sampleDelta,
                            compensationDistance);
            }
        }

        const int outputCapacity = swr_get_out_samples(m_SwrContext, inputFrames);
        if (outputCapacity <= 0 || outputCapacity > m_SampleRate) return false;
        m_CorrectedAudioBuffer.resize(
            static_cast<std::size_t>(outputCapacity) * m_ChannelCount);
        const uint8_t* inputPlanes[] = {
            reinterpret_cast<const uint8_t*>(m_AudioBuffer)
        };
        uint8_t* outputPlanes[] = {
            reinterpret_cast<uint8_t*>(m_CorrectedAudioBuffer.data())
        };
        const int outputFrames = swr_convert(m_SwrContext,
                                             outputPlanes,
                                             outputCapacity,
                                             inputPlanes,
                                             inputFrames);
        if (outputFrames < 0) {
            qCritical("PLANK audio correction failed");
            return false;
        }
        queuedBuffer = m_CorrectedAudioBuffer.data();
        queuedBytes = outputFrames * m_BytesPerSampleFrame;
    }

    m_RawAudioFrames += inputFrames;
    if (m_LastTimestampTelemetry == 0 || observedAt - m_LastTimestampTelemetry >= 1000) {
        qInfo("PLANK A/V source timing: observe_ms=%llu common=%d valid=%d audio_us=%lld video_us=%lld estimated_lead_us=%lld queue_ms=%d device_ms=%d resampler_us=%lld gaps=%llu gap_us=%lld correction_ppm=%d",
                    static_cast<unsigned long long>(observedAt), m_CommonAudioVideoEpoch, timing.phaseValid,
                    static_cast<long long>(timing.sourceUs), static_cast<long long>(timing.videoUs),
                    static_cast<long long>(timing.estimatedLeadUs), queuedMs, m_DeviceBufferDurationMs,
                    static_cast<long long>(resamplerDelayUs),
                    static_cast<unsigned long long>(timing.discontinuities),
                    static_cast<long long>(timing.sourceGapUs), getAudioClockCorrectionPpm());
        m_LastTimestampTelemetry = observedAt;
        qInfo("PLANK audio output demand: pulls=%llu shortage_requests=%llu missing_input_bytes=%llu request_us=%d headroom_us=%d pull_age_ms=%u acceleration_blocked=%d",
                    static_cast<unsigned long long>(playback.pulls),
                    static_cast<unsigned long long>(playback.shortageRequests),
                    static_cast<unsigned long long>(playback.missingInputBytes),
                    playback.requestUs, playback.headroomUs,
                    static_cast<quint32>(observedAt) - playback.ticks,
                    m_CommonAudioVideoEpoch && m_AudioPhaseController.accelerationBlocked());
    }
    if (!m_Ring->push(static_cast<const float*>(queuedBuffer), queuedBytes / m_BytesPerSampleFrame)) return false;
    if (m_EnableAvSyncCorrection) {
        m_LastSubmittedAudioMediaTimeMs =
            m_SubmittedAudioFrames * 1000 / m_SampleRate;
        m_SubmittedAudioFrames += queuedBytes / m_BytesPerSampleFrame;
    }

    return true;
}

int CoreAudioRenderer::getCapabilities()
{
    // Keep audio submission on its worker: Core Audio backpressure can wait.
    return CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
}

int CoreAudioRenderer::getQueuedAudioDurationMs()
{
    return m_Ring && m_SampleRate ? int(m_Ring->queued() * 1000ULL / m_SampleRate) : -1;
}

int CoreAudioRenderer::getDeviceBufferDurationMs()
{
    return m_DeviceBufferDurationMs;
}

qint64 CoreAudioRenderer::getSubmittedAudioMediaTimeMs()
{
    return m_LastSubmittedAudioMediaTimeMs;
}

int CoreAudioRenderer::getAudioClockCorrectionPpm()
{
    return m_CommonAudioVideoEpoch ? m_AudioPhaseController.correctionPpm() :
                                    m_AudioRateController.correctionPpm();
}
IAudioRenderer::AudioFormat CoreAudioRenderer::getAudioBufferFormat()
{
    return AudioFormat::Float32NE;
}
