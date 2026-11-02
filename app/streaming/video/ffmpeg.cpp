#include <QtGlobal>
#include <QDebug>
#include "../monotonicclock.h"
#include <Limelight.h>
#include "ffmpeg.h"
#include "decodepump.h"
#include "applevideoprofile.h"
#include "applevideo-test-frame.h"
#include "streaming/session.h"
#include "streaming/planktoolbarstats.h"

#include <h264_stream.h>
#include <QThread>
#include <QMap>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

// This is gross but it allows us to use sizeof()
#include "ffmpeg-testframes.h"

#define MAX_SPS_EXTRA_SIZE 16

#define FAILED_DECODES_RESET_THRESHOLD 20

// Note: This is NOT an exhaustive list of all decoders
// that Moonlight could pick. It will pick any working
// decoder that matches the codec ID and outputs one of
// the pixel formats that we have a renderer for.
static const QMap<QString, int> k_NonHwaccelCodecInfo = {
    // H.264
    {"h264_mmal", 0},
    {"h264_rkmpp", 0},
    {"h264_nvv4l2", 0},
    {"h264_nvmpi", 0},
    {"h264_v4l2m2m", 0},
    {"h264_omx", 0},

    // HEVC
    {"hevc_rkmpp", 0},
    {"hevc_nvv4l2", CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC},
    {"hevc_nvmpi", 0},
    {"hevc_v4l2m2m", 0},
    {"hevc_omx", 0},
};

static const AVPixelFormat* getSupportedPixelFormats(const AVCodec* decoder)
{
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void* formats = nullptr;
    if (avcodec_get_supported_config(nullptr, decoder,
                                     AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     &formats, nullptr) < 0) {
        return nullptr;
    }
    return static_cast<const AVPixelFormat*>(formats);
#else
    return decoder->pix_fmts;
#endif
}

bool FFmpegVideoDecoder::isHardwareAccelerated()
{
    return m_HwDecodeCfg != nullptr ||
            (getAVCodecCapabilities(m_VideoDecoderCtx->codec) & AV_CODEC_CAP_HARDWARE) != 0;
}

bool FFmpegVideoDecoder::isAlwaysFullScreen()
{
    return m_BackendRenderer->getRendererAttributes() & RENDERER_ATTRIBUTE_FULLSCREEN_ONLY;
}

bool FFmpegVideoDecoder::isHdrSupported()
{
    return m_BackendRenderer->getRendererAttributes() & RENDERER_ATTRIBUTE_HDR_SUPPORT;
}

void FFmpegVideoDecoder::setHdrMode(bool enabled)
{
    m_BackendRenderer->setHdrMode(enabled);
}

bool FFmpegVideoDecoder::suspendForReconnect()
{
    if (m_TestOnly || !m_DecoderThread.joinable() || m_VideoDecoderCtx == nullptr) {
        return false;
    }

    m_DecoderThreadShouldQuit.store(true);
    LiWakeWaitForVideoFrame();
    m_DecoderThread.join();
    m_DecoderThreadShouldQuit.store(false);

    if (m_BackendRenderer != nullptr) m_BackendRenderer->discardPendingFrames();
    avcodec_flush_buffers(m_VideoDecoderCtx);
    m_AwaitingKeyframe = true;
    m_LastFrameNumber = 0;
    m_ConsecutiveFailedDecodes = 0;
    m_FrameInfoQueue.clear();

    qInfo("Paused FFmpeg decode while retaining the stream renderer");
    return true;
}

bool FFmpegVideoDecoder::resumeAfterReconnect()
{
    if (m_TestOnly || m_DecoderThread.joinable() || m_VideoDecoderCtx == nullptr ||
            m_BackendRenderer == nullptr) {
        return false;
    }

    if (!m_BackendRenderer->resumePresentation()) return false;

    if (!startDecoderThread()) return false;
    qInfo("Resumed FFmpeg decode with the existing stream renderer");
    return true;
}

int FFmpegVideoDecoder::getDecoderCapabilities()
{
    bool ok;

    int capabilities = qEnvironmentVariableIntValue("DECODER_CAPS", &ok);
    if (ok) {
        qWarning("Using decoder capability override: 0x%x",
                    capabilities);
    }
    else {
        // Start with the backend renderer's capabilities
        capabilities = m_BackendRenderer->getDecoderCapabilities();

        if (!isHardwareAccelerated()) {
            // Slice up to 4 times for parallel CPU decoding, once slice per core
            int slices = qMin(MAX_SLICES, qMax(1, QThread::idealThreadCount()));
            qInfo("Encoder configured for %d slices per frame",
                        slices);
            capabilities |= CAPABILITY_SLICES_PER_FRAME(slices);

            // Enable HEVC RFI when using the FFmpeg software decoder
            capabilities |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
        }
        else if (m_HwDecodeCfg == nullptr) {
            // Non-hwaccel hardware decoders advertise capabilities by codec name.
            capabilities = k_NonHwaccelCodecInfo.value(m_VideoDecoderCtx->codec->name, 0);
            qInfo("Using capabilities table for decoder: %s -> %d",
                        m_VideoDecoderCtx->codec->name,
                        capabilities);
        }
    }

    // We use our own decoder thread with the "pull" model. This cannot
    // be overridden using the by the user because it is critical to
    // our operation.
    capabilities |= CAPABILITY_PULL_RENDERER;

    return capabilities;
}

int FFmpegVideoDecoder::getDecoderColorspace()
{
    return m_IdentityGbrEnabled ? COLORSPACE_IDENTITY_GBR :
                                  m_BackendRenderer->getDecoderColorspace();
}

int FFmpegVideoDecoder::getDecoderColorRange()
{
    return m_IdentityGbrEnabled ? COLOR_RANGE_FULL :
                                  m_BackendRenderer->getDecoderColorRange();
}

QSize FFmpegVideoDecoder::getDecoderMaxResolution()
{
    if (m_BackendRenderer->getRendererAttributes() & RENDERER_ATTRIBUTE_1080P_MAX) {
        return QSize(1920, 1080);
    }
    else {
        // No known maximum
        return QSize(0, 0);
    }
}

enum AVPixelFormat FFmpegVideoDecoder::ffGetFormat(AVCodecContext* context,
                                                   const enum AVPixelFormat* pixFmts)
{
    FFmpegVideoDecoder* decoder = (FFmpegVideoDecoder*)context->opaque;
    const enum AVPixelFormat *p;

    for (p = pixFmts; *p != -1; p++) {
        // Only match our hardware decoding codec or preferred SW pixel
        // format (if not using hardware decoding). It's crucial
        // to override the default get_format() which will try
        // to gracefully fall back to software decode and break us.
        if (*p == (decoder->m_HwDecodeCfg ? decoder->m_HwDecodeCfg->pix_fmt : context->pix_fmt) &&
                decoder->m_BackendRenderer->prepareDecoderContextInGetFormat(context, *p)) {
            return *p;
        }
    }

    // Failed to match the preferred pixel formats. Try non-preferred pixel format options
    // for non-hwaccel decoders if we didn't have a required pixel format to use.
    if (decoder->m_HwDecodeCfg == nullptr && decoder->m_RequiredPixelFormat == AV_PIX_FMT_NONE) {
        for (p = pixFmts; *p != -1; p++) {
            if (decoder->m_BackendRenderer->isPixelFormatSupported(decoder->m_VideoFormat, *p) &&
                    decoder->m_BackendRenderer->prepareDecoderContextInGetFormat(context, *p)) {
                return *p;
            }
        }
    }

    return AV_PIX_FMT_NONE;
}

FFmpegVideoDecoder::FFmpegVideoDecoder(bool testOnly)
    : m_Pkt(av_packet_alloc()),
      m_VideoDecoderCtx(nullptr),
      m_RequiredPixelFormat(AV_PIX_FMT_NONE),
      m_DecodeBuffer(1024 * 1024, 0),
      m_HwDecodeCfg(nullptr),
      m_BackendRenderer(nullptr),
      m_ConsecutiveFailedDecodes(0),
      m_AwaitingKeyframe(true),
      m_LastFrameNumber(0),
      m_StreamFps(0),
      m_VideoFormat(0),
      m_CaptureSource(DecoderCaptureSource::Nvfbc8Bit),
      m_IdentityGbrEnabled(false),
      m_NeedsSpsFixup(false),
      m_TestOnly(testOnly)
{
    m_ActiveWndVideoStats = {};
    m_LastWndVideoStats = {};
    m_GlobalVideoStats = {};

    m_DecoderThreadShouldQuit.store(false);

}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    reset();

    // Set log level back to default.
    // NB: We don't do this in reset() because we want
    // to preserve the log level across reset() during
    // test initialization.
    av_log_set_level(AV_LOG_INFO);

    av_packet_free(&m_Pkt);
}

IFFmpegRenderer* FFmpegVideoDecoder::getBackendRenderer()
{
    return m_BackendRenderer;
}

void FFmpegVideoDecoder::reset()
{
    // Terminate the decoder thread before doing anything else.
    // It might be touching things we're about to free.
    if (m_DecoderThread.joinable()) {
        m_DecoderThreadShouldQuit.store(true);
        LiWakeWaitForVideoFrame();
        m_DecoderThread.join();
        m_DecoderThreadShouldQuit.store(false);
    }

    m_AwaitingKeyframe = true;
    m_FrameInfoQueue.clear();

    m_FrameFlowTrace.flush();

    // The codec references backend hardware objects, so release it first.
    avcodec_free_context(&m_VideoDecoderCtx);
    delete m_BackendRenderer;
    m_BackendRenderer = nullptr;

    if (!m_TestOnly) {
        logVideoStats(m_GlobalVideoStats, "Global video stats");
    }
    else {
        // Test-only decoders can't have any frames submitted
        Q_ASSERT(m_GlobalVideoStats.totalFrames == 0);
    }
}

bool FFmpegVideoDecoder::completeInitialization(const AVCodec* decoder, enum AVPixelFormat requiredFormat, PDECODER_PARAMETERS params, bool testFrame)
{
    // In test-only mode, we should only see test frames
    Q_ASSERT(!m_TestOnly || testFrame);

    m_RequiredPixelFormat = requiredFormat;
    m_StreamFps = params->frameRate;
    m_VideoFormat = params->videoFormat;
    m_IdentityGbrEnabled = params->enableIdentityGbr;

    m_VideoDecoderCtx = avcodec_alloc_context3(decoder);
    if (!m_VideoDecoderCtx) {
        qCritical("Unable to allocate video decoder context");
        return false;
    }

    // Always request low delay decoding
    m_VideoDecoderCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // Allow display of corrupt frames and frames missing references
    m_VideoDecoderCtx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    m_VideoDecoderCtx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;

    // Report decoding errors to allow us to request a key frame
    //
    // With HEVC streams, FFmpeg can drop a frame (hwaccel->start_frame() fails)
    // without telling us. Since we have an infinite GOP length, this causes artifacts
    // on screen that persist for a long time. It's easy to cause this condition
    // by using NVDEC and delaying 100 ms randomly in the render path so the decoder
    // runs out of output buffers.
    m_VideoDecoderCtx->err_recognition = AV_EF_EXPLODE;

    // Enable slice multi-threading for software decoding
    if (!isHardwareAccelerated()) {
        m_VideoDecoderCtx->thread_type = FF_THREAD_SLICE;
        m_VideoDecoderCtx->thread_count = qMin(MAX_SLICES, qMax(1, QThread::idealThreadCount()));
    }
    else {
        // No threading for HW decode
        m_VideoDecoderCtx->thread_count = 1;
    }

    // Setup decoding parameters
    m_VideoDecoderCtx->width = params->width;
    m_VideoDecoderCtx->height = params->height;
    m_VideoDecoderCtx->pix_fmt = requiredFormat != AV_PIX_FMT_NONE ? requiredFormat : m_BackendRenderer->getPreferredPixelFormat(params->videoFormat);
    m_VideoDecoderCtx->get_format = ffGetFormat;

    AVDictionary* options = nullptr;

    // Allow the backend renderer to attach data to this decoder
    if (!m_BackendRenderer->prepareDecoderContext(m_VideoDecoderCtx, &options)) {
        return false;
    }

    // Nobody must override our ffGetFormat
    Q_ASSERT(m_VideoDecoderCtx->get_format == ffGetFormat);

    // Stash a pointer to this object in the context
    Q_ASSERT(m_VideoDecoderCtx->opaque == nullptr);
    m_VideoDecoderCtx->opaque = this;
    if (m_BackendRenderer->usesCustomDecodeBuffers()) {
        m_VideoDecoderCtx->get_buffer2 = [](AVCodecContext* context, AVFrame* frame, int flags) {
            const auto decoder = static_cast<FFmpegVideoDecoder*>(context->opaque);
            return decoder->m_BackendRenderer->getDecodeBuffer(context, frame, flags);
        };
    }

    int err = avcodec_open2(m_VideoDecoderCtx, decoder, &options);
    av_dict_free(&options);
    if (err < 0) {
        qCritical("Unable to open decoder for format: %x",
                     params->videoFormat);
        return false;
    }

    // FFMpeg doesn't completely initialize the codec until the codec
    // config data comes in. This would be too late for us to change
    // our minds on the selected video codec, so we'll do a trial run
    // now to see if things will actually work when the video stream
    // comes in.
    if (testFrame) {
        switch (params->videoFormat) {
        case VIDEO_FORMAT_H264:
            m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_H264TestFrame;
            m_Pkt->size = sizeof(PlankDecoderTestFrames::k_H264TestFrame);
            break;
        case VIDEO_FORMAT_H264_HIGH8_422:
            m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_H264High8_422TestFrame;
            m_Pkt->size = sizeof(PlankDecoderTestFrames::k_H264High8_422TestFrame);
            break;
        case VIDEO_FORMAT_H264_HIGH10_422:
            m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_H264High10_422TestFrame;
            m_Pkt->size = sizeof(PlankDecoderTestFrames::k_H264High10_422TestFrame);
            break;
        case VIDEO_FORMAT_H265:
            m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_HEVCMainTestFrame;
            m_Pkt->size = sizeof(PlankDecoderTestFrames::k_HEVCMainTestFrame);
            break;
        case VIDEO_FORMAT_H265_MAIN10:
            if (params->captureSource == DecoderCaptureSource::ScreenCaptureKit) {
                m_Pkt->data = (uint8_t*)k_AppleHEVCMain10TestFrame;
                m_Pkt->size = k_AppleHEVCMain10TestFrameSize;
            }
            else {
                m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_HEVCMain10TestFrame;
                m_Pkt->size = sizeof(PlankDecoderTestFrames::k_HEVCMain10TestFrame);
            }
            break;
        case VIDEO_FORMAT_H264_HIGH8_444:
            m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_h264High_444TestFrame;
            m_Pkt->size = sizeof(PlankDecoderTestFrames::k_h264High_444TestFrame);
            break;
        case VIDEO_FORMAT_H264_HIGH10_444:
            m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_h264High10_444TestFrame;
            m_Pkt->size = sizeof(PlankDecoderTestFrames::k_h264High10_444TestFrame);
            break;
        case VIDEO_FORMAT_H265_REXT8_444:
            if (params->enableIdentityGbr) {
                m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_HEVCRExt8_444IdentityGbrTestFrame;
                m_Pkt->size = sizeof(PlankDecoderTestFrames::k_HEVCRExt8_444IdentityGbrTestFrame);
            }
            else {
                m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_HEVCRExt8_444TestFrame;
                m_Pkt->size = sizeof(PlankDecoderTestFrames::k_HEVCRExt8_444TestFrame);
            }
            break;
        case VIDEO_FORMAT_H265_REXT10_444:
            if (params->captureSource == DecoderCaptureSource::ScreenCaptureKit) {
                m_Pkt->data = (uint8_t*)k_AppleHEVCRext10TestFrame;
                m_Pkt->size = k_AppleHEVCRext10TestFrameSize;
            }
            else if (params->enableIdentityGbr) {
                m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_HEVCRExt10_444IdentityGbrTestFrame;
                m_Pkt->size = sizeof(PlankDecoderTestFrames::k_HEVCRExt10_444IdentityGbrTestFrame);
            }
            else {
                m_Pkt->data = (uint8_t*)PlankDecoderTestFrames::k_HEVCRExt10_444TestFrame;
                m_Pkt->size = sizeof(PlankDecoderTestFrames::k_HEVCRExt10_444TestFrame);
            }
            break;
        default:
            qCritical("No test frame for format: %x",
                         params->videoFormat);
            return false;
        }

        AVFrame* frame = av_frame_alloc();
        if (!frame) {
            qCritical("Failed to allocate frame");
            return false;
        }

        // Some decoders won't output on the first frame, so we'll submit
        // a few test frames if we get an EAGAIN error.
        for (int retries = 0; retries < 5; retries++) {
            // Most FFmpeg decoders process input using a "push" model.
            // We'll see those fail here if the format is not supported.
            err = avcodec_send_packet(m_VideoDecoderCtx, m_Pkt);
            if (err < 0) {
                av_frame_free(&frame);
                char errorstring[512];
                av_strerror(err, errorstring, sizeof(errorstring));
                qWarning("Test decode failed (avcodec_send_packet): %s", errorstring);
                return false;
            }

            // A few FFmpeg decoders (h264_mmal) process here using a "pull" model.
            // Those decoders will fail here if the format is not supported.
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 28, 100)
            err = avcodec_receive_frame_flags(m_VideoDecoderCtx, frame,
                                              AV_CODEC_RECEIVE_FRAME_FLAG_SYNCHRONOUS);
#else
            err = avcodec_receive_frame(m_VideoDecoderCtx, frame);
#endif
            if (err != AVERROR(EAGAIN)) {
                // Done!
                break;
            }
        }

        if (err < 0) {
            char errorstring[512];
            av_strerror(err, errorstring, sizeof(errorstring));
            qWarning("Test decode failed (avcodec_receive_frame): %s", errorstring);
            av_frame_free(&frame);
            return false;
        }

        // Do not trust a codec-family capability claim by itself. Some hardware
        // paths accept H.264 configuration but substitute an 8-bit or 4:2:0
        // surface for a PLANK High 10 or 4:4:4 profile. Validate the
        // decoded test frame before allowing the renderer to accept the path.
        if (!validateDecodedProfileFrame(frame, params)) {
            av_frame_free(&frame);
            return false;
        }

        // Allow the renderer to do any additional validation it wants on this frame
        if (!m_BackendRenderer->testRenderFrame(frame)) {
            qWarning("Test decode failed (testRenderFrame)");
            av_frame_free(&frame);
            return false;
        }

        av_frame_free(&frame);
    }
    else {
        if ((params->videoFormat & VIDEO_FORMAT_MASK_H264) &&
                !(m_BackendRenderer->getDecoderCapabilities() & CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC)) {
            qInfo("Using H.264 SPS fixup");
            m_NeedsSpsFixup = true;
        }
        else {
            m_NeedsSpsFixup = false;
        }

        // Network state exists only for live decoding, never qualification.
        if (!startDecoderThread()) return false;
    }

    return true;
}

bool FFmpegVideoDecoder::validateDecodedProfileFrame(const AVFrame* frame,
                                                      PDECODER_PARAMETERS params)
{
    if (params->captureSource == DecoderCaptureSource::ScreenCaptureKit &&
            (params->encoderBackend != DecoderEncoderBackend::VideoToolbox ||
             (params->videoFormat != VIDEO_FORMAT_H265_MAIN10 && params->videoFormat != VIDEO_FORMAT_H265_REXT10_444) ||
             params->enableIdentityGbr ||
             !plankAppleVideoFrameMatches(frame, m_VideoDecoderCtx->profile, params->videoFormat == VIDEO_FORMAT_H265_REXT10_444))) {
        qWarning("Exact Apple profile validation failed: requires selected 10-bit chroma/profile, "
                    "full-range BT.709 matrix/primaries and sRGB transfer");
        return false;
    }
    AVPixelFormat storageFormat = static_cast<AVPixelFormat>(frame->format);
    if (frame->hw_frames_ctx != nullptr) {
        const auto* framesContext = reinterpret_cast<const AVHWFramesContext*>(
                    frame->hw_frames_ctx->data);
        if (framesContext == nullptr || framesContext->sw_format == AV_PIX_FMT_NONE) {
            qWarning("Exact profile validation failed: hardware surface storage format is unknown");
            return false;
        }
        storageFormat = framesContext->sw_format;
    }

    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(storageFormat);
    if (descriptor == nullptr || descriptor->nb_components < 3) {
        qWarning("Exact profile validation failed: unsupported decoded storage format %s",
                    av_get_pix_fmt_name(storageFormat) != nullptr ?
                        av_get_pix_fmt_name(storageFormat) : "unknown");
        return false;
    }

    int decodedDepth = 0;
    for (int component = 0; component < descriptor->nb_components; component++) {
        decodedDepth = qMax(decodedDepth, descriptor->comp[component].depth);
    }

    const int requiredDepth =
            (params->videoFormat & VIDEO_FORMAT_MASK_10BIT) ? 10 : 8;
    const int requiredLog2ChromaWidth =
            (params->videoFormat & VIDEO_FORMAT_MASK_YUV444) ? 0 : 1;
    const int requiredLog2ChromaHeight =
            (params->videoFormat &
             (VIDEO_FORMAT_MASK_YUV444 | VIDEO_FORMAT_MASK_YUV422)) ? 0 : 1;

    if (decodedDepth != requiredDepth ||
            descriptor->log2_chroma_w != requiredLog2ChromaWidth ||
            descriptor->log2_chroma_h != requiredLog2ChromaHeight) {
        qWarning("Exact profile validation rejected decoded format %s: "
                    "depth/chroma=%d/%d:%d required=%d/%d:%d",
                    av_get_pix_fmt_name(storageFormat) != nullptr ?
                        av_get_pix_fmt_name(storageFormat) : "unknown",
                    decodedDepth,
                    descriptor->log2_chroma_w,
                    descriptor->log2_chroma_h,
                    requiredDepth,
                    requiredLog2ChromaWidth,
                    requiredLog2ChromaHeight);
        return false;
    }

    if (params->enableIdentityGbr &&
            (frame->colorspace != AVCOL_SPC_RGB ||
             frame->color_range != AVCOL_RANGE_JPEG)) {
        qWarning("Exact identity GBR validation rejected decoded color metadata: "
                    "matrix=%d range=%d",
                    frame->colorspace,
                    frame->color_range);
        return false;
    }

    qInfo("Exact profile validation passed: codec-format=0x%x storage=%s "
                "depth=%d chroma=%d:%d matrix=%d range=%d decoder=%s",
                params->videoFormat,
                av_get_pix_fmt_name(storageFormat) != nullptr ?
                    av_get_pix_fmt_name(storageFormat) : "unknown",
                decodedDepth,
                descriptor->log2_chroma_w,
                descriptor->log2_chroma_h,
                frame->colorspace,
                frame->color_range,
                isHardwareAccelerated() ? "hardware" : "software");
    return true;
}

void FFmpegVideoDecoder::addVideoStats(VIDEO_STATS& src, VIDEO_STATS& dst)
{
    dst.receivedVideoBytes += src.receivedVideoBytes;
    dst.receivedFrames += src.receivedFrames;
    dst.decodedFrames += src.decodedFrames;
    dst.renderedFrames += src.renderedFrames;
    dst.totalFrames += src.totalFrames;
    dst.networkDroppedFrames += src.networkDroppedFrames;
    dst.pacerDroppedFrames += src.pacerDroppedFrames;
    dst.pacingQueueDroppedFrames += src.pacingQueueDroppedFrames;
    dst.renderQueueDroppedFrames += src.renderQueueDroppedFrames;
    dst.queueOverflowDroppedFrames += src.queueOverflowDroppedFrames;
    dst.totalReassemblyTime += src.totalReassemblyTime;
    dst.totalDecodeTime += src.totalDecodeTime;
    dst.totalPacerTime += src.totalPacerTime;
    dst.totalRenderTime += src.totalRenderTime;

    if (dst.minHostProcessingLatency == 0) {
        dst.minHostProcessingLatency = src.minHostProcessingLatency;
    }
    else if (src.minHostProcessingLatency != 0) {
        dst.minHostProcessingLatency = qMin(dst.minHostProcessingLatency, src.minHostProcessingLatency);
    }
    dst.maxHostProcessingLatency = qMax(dst.maxHostProcessingLatency, src.maxHostProcessingLatency);
    dst.totalHostProcessingLatency += src.totalHostProcessingLatency;
    dst.framesWithHostProcessingLatency += src.framesWithHostProcessingLatency;

    const Session* session = Session::get();
    dst.lastRtt = session != nullptr ? session->currentNetworkRttMs() : 0;

    quint64 now = PlankClock::milliseconds();

    // Initialize the measurement start point if this is the first video stat window
    if (!dst.measurementStartTimestamp) {
        dst.measurementStartTimestamp = src.measurementStartTimestamp;
    }

    // The following code assumes the global measure was already started first
    Q_ASSERT(dst.measurementStartTimestamp <= src.measurementStartTimestamp);

    const float measurementSeconds = (float)(now - dst.measurementStartTimestamp) / 1000;
    dst.totalFps = (float)dst.totalFrames / measurementSeconds;
    dst.receivedVideoMbps = (float)(dst.receivedVideoBytes * 8) / measurementSeconds / 1000000;
    dst.receivedFps = (float)dst.receivedFrames / measurementSeconds;
    dst.decodedFps = (float)dst.decodedFrames / measurementSeconds;
    dst.renderedFps = (float)dst.renderedFrames / measurementSeconds;
}

void FFmpegVideoDecoder::stringifyVideoStats(VIDEO_STATS& stats, char* output, int length)
{
    int offset = 0;
    const char* codecString;
    const auto videoFecLoss = Session::get()->currentVideoFecLoss();
    int ret;

    // Start with an empty string
    output[offset] = 0;

    switch (m_VideoFormat)
    {
    case VIDEO_FORMAT_H264:
        codecString = "H.264";
        break;

    case VIDEO_FORMAT_H264_HIGH8_422:
        codecString = "H.264 8-bit 4:2:2";
        break;

    case VIDEO_FORMAT_H264_HIGH8_444:
        codecString = "H.264 8-bit 4:4:4";
        break;

    case VIDEO_FORMAT_H264_HIGH10_444:
        codecString = "H.264 10-bit 4:4:4";
        break;

    case VIDEO_FORMAT_H264_HIGH10_422:
        codecString = "H.264 10-bit 4:2:2";
        break;

    case VIDEO_FORMAT_H265:
        codecString = "HEVC";
        break;

    case VIDEO_FORMAT_H265_REXT8_444:
        codecString = "HEVC 8-bit 4:4:4";
        break;

    case VIDEO_FORMAT_H265_MAIN10:
        codecString = "HEVC 10-bit 4:2:0";
        break;

    case VIDEO_FORMAT_H265_REXT10_444:
        codecString = "HEVC 10-bit 4:4:4";
        break;

    default:
        Q_ASSERT(false);
        codecString = "UNKNOWN";
        break;
    }

    if (stats.receivedFps > 0) {
        if (m_VideoDecoderCtx != nullptr) {
            const char* identityMapping = m_IdentityGbrEnabled ? " RGB identity" : "";
            const char* encoderBackend =
                    m_EncoderBackend == DecoderEncoderBackend::VideoToolbox ?
                        "Apple VideoToolbox, full-range BT.709/sRGB" :
                    m_EncoderBackend == DecoderEncoderBackend::NvencDirect ?
                        "NVENC" : "x264";
            ret = snprintf(&output[offset],
                           length - offset,
                           "Video stream: %dx%d %.2f FPS (Codec: %s%s, %s)\n",
                           m_VideoDecoderCtx->width,
                           m_VideoDecoderCtx->height,
                           stats.totalFps,
                           codecString,
                           identityMapping,
                           encoderBackend);
            if (ret < 0 || ret >= length - offset) {
                Q_ASSERT(false);
                return;
            }

            offset += ret;
        }

        ret = snprintf(&output[offset],
                       length - offset,
                       "Capture source: %s\n",
                       m_CaptureSource == DecoderCaptureSource::ScreenCaptureKit ?
                           "ScreenCaptureKit" :
                       m_CaptureSource == DecoderCaptureSource::NativeX11_10Bit ?
                           "Native X11/XShm (10-bit)" :
                           "NvFBC (8-bit)");
        if (ret < 0 || ret >= length - offset) {
            Q_ASSERT(false);
            return;
        }
        offset += ret;

        if (m_IdentityGbrEnabled) {
            const int identityBitDepth =
                    (m_VideoFormat & VIDEO_FORMAT_MASK_10BIT) ? 10 : 8;
            ret = snprintf(&output[offset],
                           length - offset,
                           "Presentation precision: %d-bit RGB identity\n",
                           identityBitDepth);
            if (ret < 0 || ret >= length - offset) {
                Q_ASSERT(false);
                return;
            }

            offset += ret;
        }

        ret = snprintf(&output[offset],
                       length - offset,
                       "Incoming video bitrate: %.2f Mbps\n"
                       "Frame rate (network/decode/render): %.2f/%.2f/%.2f FPS\n",
                       stats.receivedVideoMbps,
                       stats.receivedFps,
                       stats.decodedFps,
                       stats.renderedFps);
        if (ret < 0 || ret >= length - offset) {
            Q_ASSERT(false);
            return;
        }

        offset += ret;

        char beforeFecLossString[32];
        char afterFecLossString[32];

        if (videoFecLoss.before >= 0.0f) {
            snprintf(beforeFecLossString,
                     sizeof(beforeFecLossString),
                     "%.*f%%",
                     VideoPacketLossDisplayDecimalPlaces,
                     videoFecLoss.before);
        }
        else {
            snprintf(beforeFecLossString, sizeof(beforeFecLossString), "N/A");
        }

        if (videoFecLoss.after >= 0.0f) {
            snprintf(afterFecLossString,
                     sizeof(afterFecLossString),
                     "%.*f%%",
                     VideoPacketLossDisplayDecimalPlaces,
                     videoFecLoss.after);
        }
        else {
            snprintf(afterFecLossString, sizeof(afterFecLossString), "N/A");
        }

        ret = snprintf(&output[offset],
                       length - offset,
                       "Incoming video packet loss (before/after FEC): %s/%s\n",
                       beforeFecLossString,
                       afterFecLossString);
        if (ret < 0 || ret >= length - offset) {
            Q_ASSERT(false);
            return;
        }

        offset += ret;
    }

    if (stats.framesWithHostProcessingLatency > 0) {
        ret = snprintf(&output[offset],
                       length - offset,
                       "Host processing latency min/max/average: %.1f/%.1f/%.1f ms\n",
                       (float)stats.minHostProcessingLatency / 10,
                       (float)stats.maxHostProcessingLatency / 10,
                       (float)stats.totalHostProcessingLatency / 10 / stats.framesWithHostProcessingLatency);
        if (ret < 0 || ret >= length - offset) {
            Q_ASSERT(false);
            return;
        }

        offset += ret;
    }

    if (stats.renderedFrames != 0) {
        const auto rttString = PlankToolbarStats::networkRttText(stats.lastRtt).toUtf8();

        ret = snprintf(&output[offset],
                       length - offset,
                       "Client frame queue drops (%%/render/overflow): %.2f%%/%u/%u\n"
                       "Network RTT: %s\n"
                       "Frame time (decode/queue/render incl. V-sync): %.2f/%.2f/%.2f ms\n",
                       (float)stats.pacerDroppedFrames / stats.decodedFrames * 100,
                       stats.renderQueueDroppedFrames,
                       stats.queueOverflowDroppedFrames,
                       rttString.constData(),
                       (float)stats.totalDecodeTime / stats.decodedFrames,
                       (float)stats.totalPacerTime / stats.renderedFrames,
                       (float)stats.totalRenderTime / stats.renderedFrames);
        if (ret < 0 || ret >= length - offset) {
            Q_ASSERT(false);
            return;
        }

        offset += ret;
    }
}

void FFmpegVideoDecoder::logVideoStats(VIDEO_STATS& stats, const char* title)
{
    if (stats.renderedFps > 0 || stats.renderedFrames != 0) {
        char videoStatsStr[1280];
        stringifyVideoStats(stats, videoStatsStr, sizeof(videoStatsStr));

        qInfo("%s", title);
        qInfo("----------------------------------------------------------\n%s",
                    videoStatsStr);
        if (stats.framesWithHostProcessingLatency > 0) {
            qInfo("Host processing latency p95: %.1f ms",
                        (float)getHostProcessingLatencyPercentile(95) / 10);
        }
        if (stats.decodedFrames > 0) {
            qInfo("Client decode completion latency p95/p99/max: %u/%u/%u ms",
                        getDecodeLatencyPercentile(95),
                        getDecodeLatencyPercentile(99),
                        m_MaxDecodeLatencyMs);
        }
    }
}

uint16_t FFmpegVideoDecoder::getHostProcessingLatencyPercentile(unsigned int percentile) const
{
    uint64_t sampleCount = 0;
    for (uint32_t count : m_HostProcessingLatencyHistogram) {
        sampleCount += count;
    }

    if (sampleCount == 0 || percentile == 0 || percentile > 100) {
        return 0;
    }

    const uint64_t targetRank = (sampleCount * percentile + 99) / 100;
    uint64_t cumulativeSamples = 0;
    for (size_t latency = 0; latency < m_HostProcessingLatencyHistogram.size(); latency++) {
        cumulativeSamples += m_HostProcessingLatencyHistogram[latency];
        if (cumulativeSamples >= targetRank) {
            return static_cast<uint16_t>(latency);
        }
    }

    Q_ASSERT(false);
    return 0;
}

uint16_t FFmpegVideoDecoder::getDecodeLatencyPercentile(unsigned int percentile) const
{
    uint64_t sampleCount = 0;
    for (uint32_t count : m_DecodeLatencyHistogram) {
        sampleCount += count;
    }

    if (sampleCount == 0 || percentile == 0 || percentile > 100) {
        return 0;
    }

    const uint64_t targetRank = (sampleCount * percentile + 99) / 100;
    uint64_t cumulativeSamples = 0;
    for (size_t latency = 0; latency < m_DecodeLatencyHistogram.size(); latency++) {
        cumulativeSamples += m_DecodeLatencyHistogram[latency];
        if (cumulativeSamples >= targetRank) {
            return static_cast<uint16_t>(latency);
        }
    }

    Q_ASSERT(false);
    return 0;
}

bool FFmpegVideoDecoder::tryInitializeRenderer(const AVCodec* decoder,
                                               enum AVPixelFormat requiredFormat,
                                               PDECODER_PARAMETERS params,
                                               const AVCodecHWConfig* hwConfig,
                                               IFFmpegRenderer::InitFailureReason* failureReason)
{
    Q_ASSERT(params->rendererFactory);
    Q_ASSERT(!m_BackendRenderer);
    DECODER_PARAMETERS probe = *params;
    probe.width = params->captureSource == DecoderCaptureSource::ScreenCaptureKit ? 3840 : 1280;
    probe.height = params->captureSource == DecoderCaptureSource::ScreenCaptureKit ? 2160 : 720;
    m_HwDecodeCfg = hwConfig;
    if (failureReason) *failureReason = IFFmpegRenderer::InitFailureReason::Unknown;

    m_BackendRenderer = params->rendererFactory->createRenderer(hwConfig);
    if (m_BackendRenderer) {
        const bool test = m_TestOnly || m_BackendRenderer->needsTestFrame();
        auto* initial = test ? &probe : params;
        if (m_BackendRenderer->initialize(initial) &&
                completeInitialization(decoder, requiredFormat, initial, test)) {
            if (m_TestOnly || !test) return true;
            // A successful profile-specific test is mandatory before live use.
            // Recreate the renderer at the actual requested stream dimensions.
            reset();
            m_BackendRenderer = params->rendererFactory->createRenderer(hwConfig);
            if (m_BackendRenderer && m_BackendRenderer->initialize(params) &&
                    completeInitialization(decoder, requiredFormat, params, false)) return true;
            qCritical("Decoder failed to initialize after successful profile test");
        }
    }
    if (m_BackendRenderer && failureReason) *failureReason = m_BackendRenderer->getInitFailureReason();
    reset();
    return false;
}

bool FFmpegVideoDecoder::tryInitializeRendererForUnknownDecoder(const AVCodec* decoder,
                                                                PDECODER_PARAMETERS params,
                                                                bool tryHwAccel)
{
    if (!decoder) return false;
    if (tryHwAccel) {
        for (int i = 0; const auto* config = avcodec_get_hw_config(decoder, i); ++i) {
            IFFmpegRenderer::InitFailureReason reason;
            if (tryInitializeRenderer(decoder, AV_PIX_FMT_NONE, params, config, &reason)) return true;
            if (reason == IFFmpegRenderer::InitFailureReason::NoHardwareSupport) return false;
        }
    }
    return tryInitializeRenderer(decoder, AV_PIX_FMT_NONE, params, nullptr, nullptr);
}

int FFmpegVideoDecoder::getAVCodecCapabilities(const AVCodec *codec)
{
    int caps = codec->capabilities;

    // There are a bunch of out-of-tree OMX decoder implementations
    // from various SBC manufacturers that all seem to forget to set
    // AV_CODEC_CAP_HARDWARE (probably because the upstream OMX code
    // also doesn't set it). Avoid a false decoder warning on startup
    // by setting it ourselves.
    if (QString::fromUtf8(codec->name).endsWith("_omx", Qt::CaseInsensitive)) {
        caps |= AV_CODEC_CAP_HARDWARE;
    }

    return caps;
}

bool FFmpegVideoDecoder::isDecoderMatchForParams(const AVCodec *decoder, PDECODER_PARAMETERS params)
{
    Q_ASSERT(params->videoFormat & (VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265));

#if defined(HAVE_MMAL) && !defined(ALLOW_EGL_WITH_MMAL)
    // Only enable V4L2M2M by default on non-MMAL (RPi) builds. The performance
    // of the V4L2M2M wrapper around MMAL is not enough for 1080p 60 FPS, so we
    // would rather show the missing hardware acceleration warning when the user
    // is in Full KMS mode rather than try to use a poorly performing hwaccel.
    // See discussion on https://github.com/jc-kynesim/rpi-ffmpeg/pull/25
    if (strcmp(decoder->name, "h264_v4l2m2m") == 0) {
        return false;
    }
#endif

    return ((params->videoFormat & VIDEO_FORMAT_MASK_H264) && decoder->id == AV_CODEC_ID_H264) ||
           ((params->videoFormat & VIDEO_FORMAT_MASK_H265) && decoder->id == AV_CODEC_ID_HEVC);
}

bool FFmpegVideoDecoder::tryInitializeHwAccelDecoder(PDECODER_PARAMETERS params, QSet<const AVCodec*>& terminallyFailedHardwareDecoders)
{
    const AVCodec* decoder;
    void* codecIterator;

    // Iterate through hwaccel decoders
    codecIterator = NULL;
    while ((decoder = av_codec_iterate(&codecIterator))) {
        // Skip codecs that aren't decoders
        if (!av_codec_is_decoder(decoder)) {
            continue;
        }

        // Skip decoders that don't match our decoding parameters
        if (!isDecoderMatchForParams(decoder, params)) {
            continue;
        }

        // Skip non-hwaccel hardware decoders
        if (getAVCodecCapabilities(decoder) & AV_CODEC_CAP_HARDWARE) {
            continue;
        }

        // Skip hardware decoders that have returned a terminal failure status
        if (terminallyFailedHardwareDecoders.contains(decoder)) {
            continue;
        }

        // Look for the first matching hwaccel hardware decoder
        for (int i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                // No remaining hwaccel options
                break;
            }

            // Initialize the hardware codec and submit a test frame if the renderer needs it
            IFFmpegRenderer::InitFailureReason failureReason;
            if (tryInitializeRenderer(decoder, AV_PIX_FMT_NONE, params, config, &failureReason)) {
                return true;
            }
            else if (failureReason == IFFmpegRenderer::InitFailureReason::NoHardwareSupport) {
                terminallyFailedHardwareDecoders.insert(decoder);
                qInfo("Skipping remaining hwaccels due lack of hardware support for specified codec");
                break;
            }
        }
    }

    return false;
}

bool FFmpegVideoDecoder::isZeroCopyFormat(AVPixelFormat format)
{
    const AVPixFmtDescriptor* formatDesc = av_pix_fmt_desc_get(format);
    return (formatDesc && !!(formatDesc->flags & AV_PIX_FMT_FLAG_HWACCEL));
}

bool FFmpegVideoDecoder::tryInitializeNonHwAccelDecoder(PDECODER_PARAMETERS params, bool requireZeroCopyFormat, QSet<const AVCodec*>& terminallyFailedHardwareDecoders)
{
    const AVCodec* decoder;
    void* codecIterator;

    // Iterate through non-hwaccel and non-standard hwaccel hardware decoders that have AV_CODEC_CAP_HARDWARE set
    codecIterator = NULL;
    while ((decoder = av_codec_iterate(&codecIterator))) {
        // Skip codecs that aren't decoders
        if (!av_codec_is_decoder(decoder)) {
            continue;
        }

        // Skip decoders that don't match our decoding parameters
        if (!isDecoderMatchForParams(decoder, params)) {
            continue;
        }

        // Skip software/hybrid decoders and normal hwaccel decoders (which were handled in the loop above)
        if (!(getAVCodecCapabilities(decoder) & AV_CODEC_CAP_HARDWARE)) {
            continue;
        }

        // Skip decoders without zero-copy output formats if requested
        if (requireZeroCopyFormat) {
            bool foundZeroCopyFormat = false;
            const AVPixelFormat* pixelFormats = getSupportedPixelFormats(decoder);
            for (int i = 0; pixelFormats && pixelFormats[i] != AV_PIX_FMT_NONE; i++) {
                if (isZeroCopyFormat(pixelFormats[i])) {
                    foundZeroCopyFormat = true;
                    break;
                }
            }

            if (!foundZeroCopyFormat) {
                continue;
            }
        }

        // Skip hardware decoders that have returned a terminal failure status
        if (terminallyFailedHardwareDecoders.contains(decoder)) {
            continue;
        }

        // Try to initialize this decoder both as hwaccel and non-hwaccel
        if (tryInitializeRendererForUnknownDecoder(decoder, params, true)) {
            return true;
        }
    }

    return false;
}

bool FFmpegVideoDecoder::initialize(PDECODER_PARAMETERS params)
{
    m_CaptureSource = params->captureSource;
    m_EncoderBackend = params->encoderBackend;

    // Increase log level until the first frame is decoded
    av_log_set_level(AV_LOG_DEBUG);

    const AVCodec* decoder;
    void* codecIterator;

    // Hardware is always attempted first. Each candidate must successfully
    // decode and exactly validate the selected PLANK profile before
    // it can be accepted.
    {
        QSet<const AVCodec*> terminallyFailedHardwareDecoders;

        // Try tier 1 hwaccel decoders first
        if (tryInitializeHwAccelDecoder(params, terminallyFailedHardwareDecoders)) {
            return true;
        }

        // Iterate through non-hwaccel and non-standard hwaccel hardware decoders that have AV_CODEC_CAP_HARDWARE set.
        //
        // We first try to find decoders with a hwaccel format that can be rendered without CPU copyback.
        // Failing that, we will accept a decoder that only supports copyback (or one with unknown pixfmts).
        if (tryInitializeNonHwAccelDecoder(params, true /* zero copy */, terminallyFailedHardwareDecoders)) {
            return true;
        }
        if (tryInitializeNonHwAccelDecoder(params, false /* zero copy */, terminallyFailedHardwareDecoders)) {
            return true;
        }

    }

    // Iterate through all software decoders if allowed
    if (params->selectionMode != DecoderSelectionMode::ExactHardwareOnly) {
        codecIterator = NULL;
        while ((decoder = av_codec_iterate(&codecIterator))) {
            // Skip codecs that aren't decoders
            if (!av_codec_is_decoder(decoder)) {
                continue;
            }

            // Skip decoders that don't match our decoding parameters
            if (!isDecoderMatchForParams(decoder, params)) {
                continue;
            }

            // Skip hardware decoders
            //
            // NB: We can't skip hwaccel decoders here because they can be both
            // hardware and software depending on whether an hwaccel is supplied.
            // Instead, we tell tryInitializeRendererForUnknownDecoder() not to
            // try hwaccel for this decoder.
            if (getAVCodecCapabilities(decoder) & AV_CODEC_CAP_HARDWARE) {
                continue;
            }

            // Try this decoder without hwaccel
            if (tryInitializeRendererForUnknownDecoder(decoder, params, false)) {
                return true;
            }
        }
    }

    qCritical("Unable to find working decoder for format: %x",
                 params->videoFormat);
    return false;
}

void FFmpegVideoDecoder::writeBuffer(PLENTRY entry, int& offset)
{
    if (m_NeedsSpsFixup && entry->bufferType == BUFFER_TYPE_SPS) {
        h264_stream_t* stream = h264_new();
        int nalStart, nalEnd;

        // Read the old NALU
        find_nal_unit((uint8_t*)entry->data, entry->length, &nalStart, &nalEnd);
        read_nal_unit(stream,
                      (unsigned char *)&entry->data[nalStart],
                      nalEnd - nalStart);

        Q_ASSERT(nalStart == 3 || nalStart == 4); // 3 or 4 byte Annex B start sequence
        Q_ASSERT(nalEnd == entry->length);

        // Fixup the SPS to what OS X needs to use hardware acceleration
        stream->sps->num_ref_frames = 1;
        stream->sps->vui.max_dec_frame_buffering = 1;

        int initialOffset = offset;

        // Copy the modified NALU data. This clobbers byte 0 and starts NALU data at byte 1.
        // Since it prepended one extra byte, subtract one from the returned length.
        offset += write_nal_unit(stream, (uint8_t*)&m_DecodeBuffer.data()[initialOffset + nalStart - 1],
                                 MAX_SPS_EXTRA_SIZE + entry->length - nalStart) - 1;

        // Copy the NALU prefix over from the original SPS
        memcpy(&m_DecodeBuffer.data()[initialOffset], entry->data, nalStart);
        offset += nalStart;

        h264_free(stream);
    }
    else {
        // Write the buffer as-is
        memcpy(&m_DecodeBuffer.data()[offset],
               entry->data,
               entry->length);
        offset += entry->length;
    }
}

void FFmpegVideoDecoder::decoderThreadProc()
{
    while (!m_DecoderThreadShouldQuit.load()) {
        VIDEO_FRAME_HANDLE handle;
        PDECODE_UNIT du;
        // The codec is drained before waiting. Stop/reconnect wakes this wait.
        if (!LiWaitForNextVideoFrame(&handle, &du)) continue;
        if (m_DecoderThreadShouldQuit.load()) {
            LiCompleteVideoFrame(handle, DR_OK);
            break;
        }
        LiCompleteVideoFrame(handle, submitDecodeUnit(du));
    }
}

int FFmpegVideoDecoder::drainDecodedFrames()
{
    AVFrame* frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    int count = 0;
    int result = 0;
    while (!m_DecoderThreadShouldQuit.load()) {
        result = avcodec_receive_frame(m_VideoDecoderCtx, frame);
        if (result < 0) break;
        if (m_CaptureSource == DecoderCaptureSource::ScreenCaptureKit &&
                !plankAppleVideoFrameMatches(frame, m_VideoDecoderCtx->profile,
                                             m_VideoFormat == VIDEO_FORMAT_H265_REXT10_444)) {
            qCritical("Mac stream changed its negotiated profile/chroma/color format; disconnecting");
            m_DecoderThreadShouldQuit.store(true);
            Session::requestSessionQuit();
            result = AVERROR_INVALIDDATA;
            break;
        }
        if (m_FrameInfoQueue.isEmpty()) {
            // Never attribute an unaccounted output to a later input packet.
            result = AVERROR_INVALIDDATA;
            break;
        }
        presentDecodedFrame(frame);
        av_frame_unref(frame);
        count++;
    }
    av_frame_free(&frame);
    if (m_DecoderThreadShouldQuit.load()) return AVERROR_EXIT;
    return result == AVERROR(EAGAIN) ? count : result;
}

void FFmpegVideoDecoder::presentDecodedFrame(AVFrame* frame)
{
    // Attach HDR metadata to the frame if it's not already present. We will defer to
    // any metadata contained in the bitstream itself since that is guaranteed to be
    // correctly synchronized to each frame, unlike our async HDR metadata message.
    SS_HDR_METADATA hdrMetadata;
    if (LiGetHdrMetadata(&hdrMetadata)) {
        if (av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) == nullptr) {
            auto mdm = av_mastering_display_metadata_create_side_data(frame);

            mdm->display_primaries[0][0] = av_make_q(hdrMetadata.displayPrimaries[0].x, 50000);
            mdm->display_primaries[0][1] = av_make_q(hdrMetadata.displayPrimaries[0].y, 50000);
            mdm->display_primaries[1][0] = av_make_q(hdrMetadata.displayPrimaries[1].x, 50000);
            mdm->display_primaries[1][1] = av_make_q(hdrMetadata.displayPrimaries[1].y, 50000);
            mdm->display_primaries[2][0] = av_make_q(hdrMetadata.displayPrimaries[2].x, 50000);
            mdm->display_primaries[2][1] = av_make_q(hdrMetadata.displayPrimaries[2].y, 50000);

            mdm->white_point[0] = av_make_q(hdrMetadata.whitePoint.x, 50000);
            mdm->white_point[1] = av_make_q(hdrMetadata.whitePoint.y, 50000);

            mdm->min_luminance = av_make_q(hdrMetadata.minDisplayLuminance, 10000);
            mdm->max_luminance = av_make_q(hdrMetadata.maxDisplayLuminance, 1);

            mdm->has_luminance = hdrMetadata.maxDisplayLuminance != 0 ? 1 : 0;
            mdm->has_primaries = hdrMetadata.displayPrimaries[0].x != 0 ? 1 : 0;
        }

        if ((hdrMetadata.maxContentLightLevel != 0 || hdrMetadata.maxFrameAverageLightLevel != 0) &&
                av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL) == nullptr) {
            auto clm = av_content_light_metadata_create_side_data(frame);

            clm->MaxCLL = hdrMetadata.maxContentLightLevel;
            clm->MaxFALL = hdrMetadata.maxFrameAverageLightLevel;
        }
    }

    // Reset failed decodes count if we reached this far
    m_ConsecutiveFailedDecodes = 0;

    // Restore default log level after a successful decode
    av_log_set_level(AV_LOG_INFO);

    // Capture a frame timestamp to measuring pacing delay
    frame->pkt_dts = PlankClock::milliseconds();

    if (!m_FrameInfoQueue.isEmpty()) {
        // Data buffers in the DU are not valid here!
        DECODE_UNIT du = m_FrameInfoQueue.dequeue();

        // Count time in avcodec_send_packet() and avcodec_receive_frame()
        // as time spent decoding. Also count time spent in the decode unit
        // queue because that's directly caused by decoder latency.
        const uint32_t decodeLatencyMs = static_cast<uint32_t>(
            (LiGetMicroseconds() - du.enqueueTimeUs) / 1000);
        m_ActiveWndVideoStats.totalDecodeTime += decodeLatencyMs;
        m_DecodeLatencyHistogram[std::min<uint32_t>(
            decodeLatencyMs,
            m_DecodeLatencyHistogram.size() - 1)]++;
        m_MaxDecodeLatencyMs = std::max(m_MaxDecodeLatencyMs, decodeLatencyMs);

        // Store the presentation time
        frame->pts = du.presentationTimeUs / 1000;
        m_FrameFlowTrace.record(ClientFrameFlowTrace::Decode,
                               du.presentationTimeUs, du.frameNumber,
                               du.frameType == FRAME_TYPE_IDR, du.fullLength,
                               m_FrameInfoQueue.count(),
                               uint64_t(decodeLatencyMs) * 1000000);
    }

    m_ActiveWndVideoStats.decodedFrames++;

    // The Qt presenter owns the bounded render queue.
    m_BackendRenderer->renderFrame(frame);
}

int FFmpegVideoDecoder::submitDecodeUnit(PDECODE_UNIT du)
{
    PLENTRY entry = du->bufferList;
    int err;

    Q_ASSERT(!m_TestOnly);

    // If this is the first frame, reject anything that's not an IDR frame
    if (m_AwaitingKeyframe && du->frameType != FRAME_TYPE_IDR) {
        return DR_NEED_IDR;
    }

    if (!m_LastFrameNumber) {
        m_ActiveWndVideoStats.measurementStartTimestamp = PlankClock::milliseconds();
        m_LastFrameNumber = du->frameNumber;
    }
    else {
        // Any frame number greater than m_LastFrameNumber + 1 represents a dropped frame
        m_ActiveWndVideoStats.networkDroppedFrames += du->frameNumber - (m_LastFrameNumber + 1);
        m_ActiveWndVideoStats.totalFrames += du->frameNumber - (m_LastFrameNumber + 1);
        m_LastFrameNumber = du->frameNumber;
    }

    // Flip stats windows roughly every second
    if (PlankClock::milliseconds() >= m_ActiveWndVideoStats.measurementStartTimestamp + 1000) {
        m_BackendRenderer->collectPresentationStats(m_ActiveWndVideoStats);
        VIDEO_STATS lastTwoWndStats = {};
        addVideoStats(m_LastWndVideoStats, lastTwoWndStats);
        addVideoStats(m_ActiveWndVideoStats, lastTwoWndStats);
        VIDEO_STATS activeWndStats = {};
        addVideoStats(m_ActiveWndVideoStats, activeWndStats);
        const float currentVideoMbps = activeWndStats.receivedVideoMbps;
        // Incoming video bitrate has one calculation and one value for every
        // UI consumer. The remaining debug statistics retain the established
        // two-window smoothing in lastTwoWndStats.
        lastTwoWndStats.receivedVideoMbps = currentVideoMbps;
        Session::get()->updateRenderedStats(lastTwoWndStats.renderedFps,
                                            currentVideoMbps);

        char text[2048];
        stringifyVideoStats(lastTwoWndStats, text, sizeof(text));
        emit Session::get()->qtStatisticsUpdated(QString::fromUtf8(text));

        // Accumulate these values into the global stats
        addVideoStats(m_ActiveWndVideoStats, m_GlobalVideoStats);

        // Move this window into the last window slot and clear it for next window
        m_LastWndVideoStats = m_ActiveWndVideoStats;
        m_ActiveWndVideoStats = {};
        m_ActiveWndVideoStats.measurementStartTimestamp = PlankClock::milliseconds();
    }

    if (du->frameHostProcessingLatency != 0) {
        if (m_ActiveWndVideoStats.minHostProcessingLatency != 0) {
            m_ActiveWndVideoStats.minHostProcessingLatency = qMin(m_ActiveWndVideoStats.minHostProcessingLatency, du->frameHostProcessingLatency);
        }
        else {
            m_ActiveWndVideoStats.minHostProcessingLatency = du->frameHostProcessingLatency;
        }
        m_ActiveWndVideoStats.framesWithHostProcessingLatency += 1;
        m_HostProcessingLatencyHistogram[du->frameHostProcessingLatency] += 1;
    }
    m_ActiveWndVideoStats.maxHostProcessingLatency = qMax(m_ActiveWndVideoStats.maxHostProcessingLatency, du->frameHostProcessingLatency);
    m_ActiveWndVideoStats.totalHostProcessingLatency += du->frameHostProcessingLatency;

    m_ActiveWndVideoStats.receivedFrames++;
    m_ActiveWndVideoStats.receivedVideoBytes += static_cast<uint64_t>(du->fullLength);
    m_ActiveWndVideoStats.totalFrames++;

    av_packet_unref(m_Pkt);
    const auto* contiguous = LiVideoFrameData(du);
    if (contiguous && (!m_NeedsSpsFixup || du->frameType != FRAME_TYPE_IDR)) {
        static_assert(PLANK_VIDEO_BUFFER_PADDING >= AV_INPUT_BUFFER_PADDING_SIZE);
        LiRetainVideoBuffer(du->frameBuffer);
        m_Pkt->buf = av_buffer_create(const_cast<uint8_t*>(contiguous),
            du->fullLength + AV_INPUT_BUFFER_PADDING_SIZE,
            [](void* owner, uint8_t*) { LiReleaseVideoBuffer(static_cast<PLANK_VIDEO_BUFFER*>(owner)); },
            du->frameBuffer, AV_BUFFER_FLAG_READONLY);
        if (!m_Pkt->buf) {
            LiReleaseVideoBuffer(du->frameBuffer);
            return DR_NEED_IDR;
        }
        m_Pkt->data = m_Pkt->buf->data;
        m_Pkt->size = du->fullLength;
    } else {
        // SPS rewriting or removed noncontiguous prefix NALs need assembly.
        // Keep this exception explicit; ordinary frames share the receive bytes.
        const int extra = du->frameType == FRAME_TYPE_IDR ? MAX_SPS_EXTRA_SIZE : 0;
        m_DecodeBuffer.resize(du->fullLength + extra + AV_INPUT_BUFFER_PADDING_SIZE);
        int offset = 0;
        while (entry != nullptr) {
            writeBuffer(entry, offset);
            entry = entry->next;
        }
        memset(m_DecodeBuffer.data() + offset, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        m_Pkt->data = reinterpret_cast<uint8_t*>(m_DecodeBuffer.data());
        m_Pkt->size = offset;
    }

    if (du->frameType == FRAME_TYPE_IDR) {
        m_Pkt->flags = AV_PKT_FLAG_KEY;
    }
    else {
        m_Pkt->flags = 0;
    }

    m_ActiveWndVideoStats.totalReassemblyTime += static_cast<uint32_t>(
        (du->enqueueTimeUs - du->receiveTimeUs) / 1000);

    err = plankSendAndDrain(AVERROR(EAGAIN), AVERROR_INVALIDDATA,
        [this] { return avcodec_send_packet(m_VideoDecoderCtx, m_Pkt); },
        [this] { return drainDecodedFrames(); },
        [this, du] {
            DECODE_UNIT metadata = *du;
            metadata.bufferList = nullptr;
            metadata.frameBuffer = nullptr;
            m_FrameInfoQueue.enqueue(metadata);
            m_AwaitingKeyframe = false;
        });
    av_packet_unref(m_Pkt);
    if (err < 0) {
        if (m_DecoderThreadShouldQuit.load()) return DR_OK;
        char errorstring[512];
        av_strerror(err, errorstring, sizeof(errorstring));
        qWarning("Video decode failed: %s (frame %d)", errorstring, du->frameNumber);
        // A codec error can discard an unknown number of delayed outputs.
        // Flush both sides together so old metadata cannot label a new frame.
        avcodec_flush_buffers(m_VideoDecoderCtx);
        m_FrameInfoQueue.clear();
        m_AwaitingKeyframe = true;
        if (++m_ConsecutiveFailedDecodes == FAILED_DECODES_RESET_THRESHOLD) {
            qCritical("Resetting decoder due to consistent failure");
            Session::requestDecoderReset();
            m_DecoderThreadShouldQuit.store(true);
        }
        return DR_NEED_IDR;
    }
    return DR_OK;
}

// Owned by the session worker. Stop is published before waking the transport
// wait; joining precedes any codec or renderer destruction.
bool FFmpegVideoDecoder::startDecoderThread()
{
    Q_ASSERT(!m_DecoderThread.joinable());
    m_DecoderThreadShouldQuit.store(false);
    try {
        m_DecoderThread = std::thread([this] {
            QThread::currentThread()->setObjectName(QStringLiteral("FFDecoder"));
            decoderThreadProc();
        });
        return true;
    } catch (const std::system_error& error) {
        qCritical("Unable to create decoder thread: %s", error.what());
        return false;
    }
}
