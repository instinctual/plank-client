#pragma once

#include <QStringList>

#include "streaming/video/decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>

}

class IFFmpegRenderer {
public:
    virtual ~IFFmpegRenderer() = default;
    virtual bool initialize(PDECODER_PARAMETERS params) = 0;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) = 0;
    virtual void renderFrame(AVFrame* frame) = 0;

    virtual void discardPendingFrames() {}
    virtual bool resumePresentation() { return true; }
    virtual void collectPresentationStats(VIDEO_STATS&) {}
    virtual bool usesCustomDecodeBuffers() const { return false; }
    virtual int getDecodeBuffer(AVCodecContext* context, AVFrame* frame, int flags) {
        return avcodec_default_get_buffer2(context, frame, flags);
    }

    // Exact-format decoding and presentation are mandatory before live use.
    virtual bool testRenderFrame(AVFrame*) = 0;

    virtual int getDecoderCapabilities() {
        // No special capabilities by default
        return 0;
    }

    virtual int getDecoderColorspace() {
        // Rec 601 is default
        return COLORSPACE_REC_601;
    }

    virtual int getDecoderColorRange() {
        // Limited is the default
        return COLOR_RANGE_LIMITED;
    }

    virtual AVPixelFormat getPreferredPixelFormat(int videoFormat) {
        if (videoFormat & VIDEO_FORMAT_MASK_10BIT) {
            return (videoFormat & VIDEO_FORMAT_MASK_YUV444) ?
                AV_PIX_FMT_YUV444P10 : // 10-bit 3-plane YUV 4:4:4
                (videoFormat & VIDEO_FORMAT_MASK_YUV422) ?
                    AV_PIX_FMT_YUV422P10 : // 10-bit 3-plane YUV 4:2:2
                    AV_PIX_FMT_P010;       // 10-bit 2-plane YUV 4:2:0
        }
        else {
            return (videoFormat & VIDEO_FORMAT_MASK_YUV444) ?
                       AV_PIX_FMT_YUV444P : // 8-bit 3-plane YUV 4:4:4
                       (videoFormat & VIDEO_FORMAT_MASK_YUV422) ?
                           AV_PIX_FMT_YUV422P : // 8-bit 3-plane YUV 4:2:2
                           AV_PIX_FMT_YUV420P;  // 8-bit 3-plane YUV 4:2:0
        }
    }

    virtual bool isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat) {
        // By default, we only support the preferred pixel format
        return getPreferredPixelFormat(videoFormat) == pixelFormat;
    }

    virtual bool prepareDecoderContextInGetFormat(AVCodecContext*, AVPixelFormat) {
        // Assume no further initialization is required
        return true;
    }


};

// Window-independent decoder selection boundary. The existing exact-profile
// test decode is still mandatory. Returning null rejects only this backend;
// it does not authorize another profile or a CPU-copy presentation fallback.
class IFFmpegRendererFactory
{
public:
    virtual ~IFFmpegRendererFactory() = default;
    virtual IFFmpegRenderer* createRenderer(const AVCodecHWConfig* hardware) = 0;
};
