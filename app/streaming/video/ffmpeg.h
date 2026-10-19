#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <QQueue>

#include "decoder.h"
#include "ffmpeg-renderers/renderer.h"
#include "ffmpeg-renderers/pacer/pacer.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class FFmpegVideoDecoder : public IVideoDecoder {
public:
    FFmpegVideoDecoder(bool testOnly);
    virtual ~FFmpegVideoDecoder() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool isHardwareAccelerated() override;
    virtual bool isAlwaysFullScreen() override;
    virtual bool isHdrSupported() override;
    virtual int getDecoderCapabilities() override;
    virtual int getDecoderColorspace() override;
    virtual int getDecoderColorRange() override;
    virtual QSize getDecoderMaxResolution() override;
    virtual int submitDecodeUnit(PDECODE_UNIT du) override;
    virtual void renderFrameOnMainThread() override;
    virtual void setHdrMode(bool enabled) override;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;
    virtual bool suspendForReconnect() override;
    virtual bool resumeAfterReconnect() override;

    virtual IFFmpegRenderer* getBackendRenderer();

private:
    ClientFrameFlowTrace m_FrameFlowTrace {"decode"};
    bool completeInitialization(const AVCodec* decoder,
                                enum AVPixelFormat requiredFormat,
                                PDECODER_PARAMETERS params,
                                bool testFrame,
                                bool useAlternateFrontend);

    bool validateDecodedProfileFrame(const AVFrame* frame,
                                     PDECODER_PARAMETERS params);

    void stringifyVideoStats(VIDEO_STATS& stats, char* output, int length);

    void logVideoStats(VIDEO_STATS& stats, const char* title);

    uint16_t getHostProcessingLatencyPercentile(unsigned int percentile) const;

    uint16_t getDecodeLatencyPercentile(unsigned int percentile) const;

    void addVideoStats(VIDEO_STATS& src, VIDEO_STATS& dst);

    bool createFrontendRenderer(PDECODER_PARAMETERS params, bool useAlternateFrontend);

    static
    bool isDecoderMatchForParams(const AVCodec *decoder, PDECODER_PARAMETERS params);

    static
    bool isZeroCopyFormat(AVPixelFormat format);

    static
    int getAVCodecCapabilities(const AVCodec *codec);

    bool tryInitializeHwAccelDecoder(PDECODER_PARAMETERS params,
                                     int pass,
                                     QSet<const AVCodec*>& terminallyFailedHardwareDecoders);

    bool tryInitializeNonHwAccelDecoder(PDECODER_PARAMETERS params,
                                        bool requireZeroCopyFormat,
                                        QSet<const AVCodec*>& terminallyFailedHardwareDecoders);

    bool tryInitializeRendererForUnknownDecoder(const AVCodec* decoder,
                                                PDECODER_PARAMETERS params,
                                                bool tryHwAccel);

    bool tryInitializeRenderer(const AVCodec* decoder,
                               enum AVPixelFormat requiredFormat,
                               PDECODER_PARAMETERS params,
                               const AVCodecHWConfig* hwConfig,
                               IFFmpegRenderer::InitFailureReason* failureReason,
                               std::function<IFFmpegRenderer*()> createRendererFunc);

    static IFFmpegRenderer* createHwAccelRenderer(const AVCodecHWConfig* hwDecodeCfg, int pass);

    void reset();

    void writeBuffer(PLENTRY entry, int& offset);

    static
    enum AVPixelFormat ffGetFormat(AVCodecContext* context,
                                   const enum AVPixelFormat* pixFmts);

    void decoderThreadProc();

    static int decoderThreadProcThunk(void* context);

    AVPacket* m_Pkt;
    AVCodecContext* m_VideoDecoderCtx;
    enum AVPixelFormat m_RequiredPixelFormat;
    QByteArray m_DecodeBuffer;
    const AVCodecHWConfig* m_HwDecodeCfg;
    IFFmpegRenderer* m_BackendRenderer;
    IFFmpegRenderer* m_FrontendRenderer;
    int m_ConsecutiveFailedDecodes;
    Pacer* m_Pacer;
    VIDEO_STATS m_ActiveWndVideoStats;
    VIDEO_STATS m_LastWndVideoStats;
    VIDEO_STATS m_GlobalVideoStats;
    std::array<uint32_t, std::numeric_limits<uint16_t>::max() + 1>
        m_HostProcessingLatencyHistogram {};
    std::array<uint32_t, 1001> m_DecodeLatencyHistogram {};
    uint32_t m_MaxDecodeLatencyMs = 0;

    int m_FramesIn;
    int m_FramesOut;

    int m_LastFrameNumber;
    int m_StreamFps;
    int m_VideoFormat;
    DecoderCaptureSource m_CaptureSource;
    DecoderEncoderBackend m_EncoderBackend;
    bool m_IdentityGbrEnabled;
    bool m_NeedsSpsFixup;
    bool m_TestOnly;
    SDL_Thread* m_DecoderThread;
    SDL_AtomicInt m_DecoderThreadShouldQuit;

    // Data buffers in the queued DU are not valid
    QQueue<DECODE_UNIT> m_FrameInfoQueue;


};
