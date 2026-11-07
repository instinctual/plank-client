#include <QtGlobal>
#include <QDebug>
#include "../monotonicclock.h"
#include <Limelight.h>
#include "ffmpeg.h"
#include "decodepump.h"
#include "decodepacket.h"
#include "applevideoprofile.h"
#include "applevideo-test-frame.h"
#include "streaming/session.h"
#include "streaming/planktoolbarstats.h"

#include <QThread>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

// This is gross but it allows us to use sizeof()
#include "ffmpeg-testframes.h"


#define FAILED_DECODES_RESET_THRESHOLD 20

bool FFmpegVideoDecoder::isHardwareAccelerated()
{
    return m_HwDecodeCfg != nullptr;
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

            err = avcodec_receive_frame_flags(m_VideoDecoderCtx, frame,
                                              AV_CODEC_RECEIVE_FRAME_FLAG_SYNCHRONOUS);
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
                                               const AVCodecHWConfig* hwConfig)
{
    Q_ASSERT(params->rendererFactory);
    Q_ASSERT(!m_BackendRenderer);
    DECODER_PARAMETERS probe = *params;
    probe.width = params->captureSource == DecoderCaptureSource::ScreenCaptureKit ? 3840 : 1280;
    probe.height = params->captureSource == DecoderCaptureSource::ScreenCaptureKit ? 2160 : 720;
    m_HwDecodeCfg = hwConfig;

    m_BackendRenderer = params->rendererFactory->createRenderer(hwConfig);
    if (m_BackendRenderer) {
        if (m_BackendRenderer->initialize(&probe) &&
                completeInitialization(decoder, requiredFormat, &probe, true)) {
            if (m_TestOnly) return true;
            // A successful profile-specific test is mandatory before live use.
            // Recreate the renderer at the actual requested stream dimensions.
            reset();
            m_BackendRenderer = params->rendererFactory->createRenderer(hwConfig);
            if (m_BackendRenderer && m_BackendRenderer->initialize(params) &&
                    completeInitialization(decoder, requiredFormat, params, false)) return true;
            qCritical("Decoder failed to initialize after successful profile test");
        }
    }
    reset();
    return false;
}

bool FFmpegVideoDecoder::initialize(PDECODER_PARAMETERS params)
{
    m_CaptureSource = params->captureSource;
    m_EncoderBackend = params->encoderBackend;
    av_log_set_level(AV_LOG_DEBUG);

    const auto codecId = (params->videoFormat & VIDEO_FORMAT_MASK_H264) ?
                AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    const AVCodec* decoder = avcodec_find_decoder(codecId);
    if (!decoder) return false;

    // The Qt factory accepts the platform's VA-API or VideoToolbox backend.
    // Every attempt must decode and present the exact selected profile first.
    for (int i = 0; const auto* hardware = avcodec_get_hw_config(decoder, i); ++i) {
        if (tryInitializeRenderer(decoder, AV_PIX_FMT_NONE, params, hardware)) return true;
    }
    if (plankSoftwareDecodingAllowed(params->videoFormat) &&
            tryInitializeRenderer(decoder, AV_PIX_FMT_NONE, params, nullptr)) return true;

    if (!plankSoftwareDecodingAllowed(params->videoFormat)) {
        qCritical("The selected HEVC profile requires exact-format hardware decoding; software fallback is disabled");
    }
    qCritical("Unable to find working decoder for format: %x", params->videoFormat);
    return false;
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
        Session::get()->publishVideoMetrics(m_ActiveWndVideoStats, isHardwareAccelerated());
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

    if (!plankSetDecodePacket(m_Pkt, du, m_DecodeBuffer)) return DR_NEED_IDR;

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
