#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <QQueue>
#include <vector>

#include "decoder.h"
#include "ffmpeg-renderers/renderer.h"
#include "../clientframeflowtrace.h"
#include <thread>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
}

class FFmpegVideoDecoder : public IVideoDecoder {
public:
    FFmpegVideoDecoder(bool testOnly);
    virtual ~FFmpegVideoDecoder() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool isHardwareAccelerated() override;
    virtual int getDecoderCapabilities() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual int submitDecodeUnit(PDECODE_UNIT du) override;
    virtual bool suspendForReconnect() override;
    virtual bool resumeAfterReconnect() override;

    virtual IFFmpegRenderer* getBackendRenderer();

private:
    ClientFrameFlowTrace m_FrameFlowTrace {"decode"};
    bool completeInitialization(const AVCodec* decoder,
                                enum AVPixelFormat requiredFormat,
                                PDECODER_PARAMETERS params,
                                bool testFrame);

    bool validateDecodedProfileFrame(const AVFrame* frame,
                                     PDECODER_PARAMETERS params);

    void stringifyVideoStats(VIDEO_STATS& stats, char* output, int length);

    void logVideoStats(VIDEO_STATS& stats, const char* title);

    uint16_t getHostProcessingLatencyPercentile(unsigned int percentile) const;

    uint16_t getDecodeLatencyPercentile(unsigned int percentile) const;

    void addVideoStats(VIDEO_STATS& src, VIDEO_STATS& dst);

    bool tryInitializeRenderer(const AVCodec* decoder,
                               enum AVPixelFormat requiredFormat,
                               PDECODER_PARAMETERS params,
                               const AVCodecHWConfig* hwConfig);

    void reset();


    static
    enum AVPixelFormat ffGetFormat(AVCodecContext* context,
                                   const enum AVPixelFormat* pixFmts);

    void decoderThreadProc();
    int drainDecodedFrames();
    void presentDecodedFrame(AVFrame* frame);

    bool startDecoderThread();

    AVPacket* m_Pkt;
    AVCodecContext* m_VideoDecoderCtx;
    enum AVPixelFormat m_RequiredPixelFormat;
    std::vector<uint8_t> m_DecodeBuffer;
    const AVCodecHWConfig* m_HwDecodeCfg;
    IFFmpegRenderer* m_BackendRenderer;
    int m_ConsecutiveFailedDecodes;
    VIDEO_STATS m_ActiveWndVideoStats;
    VIDEO_STATS m_LastWndVideoStats;
    VIDEO_STATS m_GlobalVideoStats;
    std::array<uint32_t, std::numeric_limits<uint16_t>::max() + 1>
        m_HostProcessingLatencyHistogram {};
    std::array<uint32_t, 1001> m_DecodeLatencyHistogram {};
    uint32_t m_MaxDecodeLatencyMs = 0;

    bool m_AwaitingKeyframe;

    int m_LastFrameNumber;
    int m_StreamFps;
    int m_VideoFormat;
    DecoderCaptureSource m_CaptureSource;
    DecoderEncoderBackend m_EncoderBackend;
    bool m_IdentityGbrEnabled;
    bool m_TestOnly;
    std::thread m_DecoderThread;
    std::atomic_bool m_DecoderThreadShouldQuit{false};

    // Data buffers in the queued DU are not valid
    QQueue<DECODE_UNIT> m_FrameInfoQueue;

};
