#pragma once

#include <QStringList>

#include "streaming/video/decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>

}

#define RENDERER_ATTRIBUTE_FULLSCREEN_ONLY 0x01
#define RENDERER_ATTRIBUTE_1080P_MAX 0x02
#define RENDERER_ATTRIBUTE_HDR_SUPPORT 0x04
#define RENDERER_ATTRIBUTE_NO_BUFFERING 0x08
#define RENDERER_ATTRIBUTE_FORCE_PACING 0x10

class IFFmpegRenderer {
public:
    virtual ~IFFmpegRenderer() = default;
    virtual bool initialize(PDECODER_PARAMETERS params) = 0;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) = 0;
    virtual void renderFrame(AVFrame* frame) = 0;

    // Qt owns presentation scheduling and reports completion separately from
    // submission. Do not place a second Pacer queue in front of that owner or
    // publish the A/V clock when a frame has merely been queued.
    virtual bool ownsPresentationScheduling() const { return false; }
    virtual void discardPendingFrames() {}
    virtual bool resumePresentation() { return true; }
    virtual void collectPresentationStats(VIDEO_STATS&) {}
    virtual bool usesCustomDecodeBuffers() const { return false; }
    virtual int getDecodeBuffer(AVCodecContext* context, AVFrame* frame, int flags) {
        return avcodec_default_get_buffer2(context, frame, flags);
    }

    enum class InitFailureReason
    {
        Unknown,

        // Only return this reason code if the hardware physically lacks support for
        // the specified codec. If the FFmpeg decoder code sees this value, it will
        // assume trying additional hwaccel renderers will useless and give up.
        //
        // NB: This should only be used under very special circumstances for cases
        // where trying additional hwaccels may be undesirable since it could lead
        // to incorrectly skipping working hwaccels.
        NoHardwareSupport,
    };

    virtual InitFailureReason getInitFailureReason() {
        return InitFailureReason::Unknown;
    }

    // Called for threaded renderers to allow them to wait prior to us latching
    // the next frame for rendering (as opposed to waiting on buffer swap with
    // an older frame already queued for display).
    virtual void waitToRender() {
        // Don't wait by default
    }

    // Called on the same thread as renderFrame() during destruction of the renderer
    virtual void cleanupRenderContext() {
        // Nothing
    }

    virtual bool testRenderFrame(AVFrame*) {
        // If the renderer doesn't provide an explicit test routine,
        // we will always assume that any returned AVFrame can be
        // rendered successfully.
        return true;
    }

    // NOTE: This can be called BEFORE initialize()!
    virtual bool needsTestFrame() {
        // No test frame required by default
        return false;
    }

    virtual int getDecoderCapabilities() {
        // No special capabilities by default
        return 0;
    }

    virtual int getRendererAttributes() {
        // No special attributes by default
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

    virtual int getFrameColorspace(const AVFrame* frame) {
        // Prefer the colorspace field on the AVFrame itself
        switch (frame->colorspace) {
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_BT470BG:
            return COLORSPACE_REC_601;
        case AVCOL_SPC_BT709:
            return COLORSPACE_REC_709;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return COLORSPACE_REC_2020;
        case AVCOL_SPC_RGB:
            return COLORSPACE_IDENTITY_GBR;
        default:
            // If the colorspace is not populated, assume the encoder
            // is sending the colorspace that we requested.
            return getDecoderColorspace();
        }
    }

    virtual bool isFrameFullRange(const AVFrame* frame) {
        // This handles the case where the color range is unknown,
        // so that we use Limited color range which is the default
        // behavior for Moonlight.
        return frame->color_range == AVCOL_RANGE_JPEG;
    }

    virtual bool isRenderThreadSupported() {
        // Render thread is supported by default
        return true;
    }

    virtual bool isDirectRenderingSupported() {
        // The renderer can render directly to the display
        return true;
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

    virtual void setHdrMode(bool) {
        // Nothing
    }

    virtual bool prepareDecoderContextInGetFormat(AVCodecContext*, AVPixelFormat) {
        // Assume no further initialization is required
        return true;
    }

    virtual void prepareToRender() {
        // Allow renderers to perform any final preparations for
        // rendering after they have been selected to render. Such
        // preparations might include clearing the window.
    }

    // Allow renderers to expose their type
    enum class RendererType {
        Unknown,
        Vulkan
    };
    virtual RendererType getRendererType() {
        return RendererType::Unknown;
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
