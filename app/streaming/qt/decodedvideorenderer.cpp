#include "decodedvideorenderer.h"
#include "../avsynccontroller.h"

#include <QDebug>
#include <algorithm>
#include <limits>
#include <mutex>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace {
struct PresentationStats {
    std::mutex mutex;
    bool active = true;
    uint64_t presented = 0, dropped = 0, queueMs = 0, renderMs = 0;
};

class Renderer final : public IFFmpegRenderer
{
public:
    Renderer(bool hardware, QtSoftwareVideoDevice::Ptr device, QString renderNode,
             QtDecodedVideoRendererFactory::OpenDelivery open)
        : m_Hardware(hardware), m_Device(std::move(device)),
          m_RenderNode(std::move(renderNode)), m_Open(std::move(open)) {}

    ~Renderer() override
    {
        discardPendingFrames();
        av_buffer_unref(&m_HardwareDevice);
    }

    bool initialize(PDECODER_PARAMETERS params) override
    {
        if (!params || !m_Open || (!m_Hardware && !m_Device)) return false;
        m_VideoFormat = params->videoFormat;
        m_Identity = params->enableIdentityGbr;
        m_Delivery = m_Open();
        if (!m_Delivery.submit || !m_Delivery.qualify || !m_Delivery.discard || !m_Delivery.fail) return false;
        if (m_Hardware) {
            // Derived from the Qt EGL device, never a guessed card index.
            if (m_RenderNode.isEmpty()) return false;
            const auto path = m_RenderNode.toUtf8();
            if (av_hwdevice_ctx_create(&m_HardwareDevice, AV_HWDEVICE_TYPE_VAAPI,
                                      path.constData(), nullptr, 0) < 0) return false;
        }
        return true;
    }

    bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override
    {
        if (!m_Hardware) return bool(m_Device);
        context->hw_device_ctx = av_buffer_ref(m_HardwareDevice);
        // Bounded exported-surface leases (one pending + three render slots).
        context->extra_hw_frames = std::max(context->extra_hw_frames, 8);
        return context->hw_device_ctx != nullptr;
    }

    bool usesCustomDecodeBuffers() const override { return !m_Hardware; }
    int getDecodeBuffer(AVCodecContext* context, AVFrame* frame, int flags) override
    {
        return m_Device->getBuffer(context, frame, flags);
    }

    bool needsTestFrame() override { return true; }
    bool ownsPresentationScheduling() const override { return true; }
    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO) override { return true; }
    int getDecoderColorspace() override { return m_Identity ? COLORSPACE_IDENTITY_GBR : COLORSPACE_REC_709; }
    int getDecoderColorRange() override { return COLOR_RANGE_FULL; }

    AVPixelFormat getPreferredPixelFormat(int format) override
    {
        if (m_Hardware) return AV_PIX_FMT_VAAPI;
        if (m_Identity) return (format & VIDEO_FORMAT_MASK_10BIT) ? AV_PIX_FMT_GBRP10LE : AV_PIX_FMT_GBRP;
        if (format & VIDEO_FORMAT_MASK_YUV444)
            return (format & VIDEO_FORMAT_MASK_10BIT) ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P;
        if (format & VIDEO_FORMAT_MASK_YUV422)
            return (format & VIDEO_FORMAT_MASK_10BIT) ? AV_PIX_FMT_YUV422P10LE : AV_PIX_FMT_YUV422P;
        return (format & VIDEO_FORMAT_MASK_10BIT) ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    }

    bool testRenderFrame(AVFrame* frame) override
    {
        QString error;
        auto retained = retain(frame, {}, error);
        const bool accepted = retained && m_Delivery.qualify(std::move(retained), error);
        if (!accepted) qWarning().noquote() << "Qt exact-profile presentation test failed:" << error;
        return accepted;
    }

    void renderFrame(AVFrame* frame) override
    {
        if (m_FrameRejected) return;
        const auto stats = m_Stats;
        const int64_t mediaPts = frame->pts;
        QString error;
        auto retained = retain(frame, [stats, mediaPts](bool presented, uint64_t queuedMs, uint64_t renderMs) {
            std::lock_guard<std::mutex> lock(stats->mutex);
            if (!stats->active) return;
            if (presented) {
                ++stats->presented;
                stats->queueMs += queuedMs > renderMs ? queuedMs - renderMs : 0;
                stats->renderMs += renderMs;
                // Qt has submitted the primary output's swap. This is not a
                // claim of panel scan-out; it is the same presentation edge
                // used by the existing audio synchronizer, not decode arrival.
                if (mediaPts != AV_NOPTS_VALUE)
                    PlankAvSync::publishVideoClock(mediaPts, uint32_t(SDL_GetTicks()));
            } else {
                ++stats->dropped;
            }
        }, error);
        if (!retained) {
            m_FrameRejected = true;
            m_Delivery.fail(QStringLiteral("Qt could not retain the selected video format: %1").arg(error));
            std::lock_guard<std::mutex> lock(stats->mutex);
            if (stats->active) ++stats->dropped;
            return;
        }
        m_Delivery.submit(std::move(retained));
    }

    void discardPendingFrames() override
    {
        {
            std::lock_guard<std::mutex> lock(m_Stats->mutex);
            m_Stats->active = false;
        }
        if (m_Delivery.discard) m_Delivery.discard();
    }

    bool resumePresentation() override
    {
        // The decoder is stopped while this generation changes. Old frame
        // feedback retains the retired counters, never the new audio clock.
        m_Stats = std::make_shared<PresentationStats>();
        m_FrameRejected = false;
        m_Delivery = m_Open();
        return m_Delivery.submit && m_Delivery.qualify && m_Delivery.discard && m_Delivery.fail;
    }

    void collectPresentationStats(VIDEO_STATS& out) override
    {
        std::lock_guard<std::mutex> lock(m_Stats->mutex);
        const auto add = [](uint32_t& destination, uint64_t& value) {
            destination = uint32_t(std::min<uint64_t>(uint64_t(destination) + value, UINT32_MAX));
            value = 0;
        };
        add(out.renderedFrames, m_Stats->presented);
        const auto drops = std::min<uint64_t>(m_Stats->dropped, UINT32_MAX);
        out.pacerDroppedFrames = uint32_t(std::min<uint64_t>(uint64_t(out.pacerDroppedFrames) + drops, UINT32_MAX));
        add(out.renderQueueDroppedFrames, m_Stats->dropped);
        add(out.totalPacerTime, m_Stats->queueMs);
        add(out.totalRenderTime, m_Stats->renderMs);
    }

private:
    QtVideoFrame::Ptr retain(const AVFrame* frame, QtVideoFrame::Feedback feedback, QString& error)
    {
        // Both Linux and Apple codec contracts require full-range samples.
        // The legacy STREAM_CONFIGURATION colorRange field is not proof of
        // the actual bitstream; FFmpeg's exact-profile test validates it.
        if (!frame || frame->color_range != AVCOL_RANGE_JPEG ||
                frame->colorspace != (m_Identity ? AVCOL_SPC_RGB : AVCOL_SPC_BT709)) {
            error = QStringLiteral("Decoded range/matrix differs from the selected profile");
            return {};
        }
        if (m_Hardware) {
            const bool allowP010 = !m_Identity && (m_VideoFormat & VIDEO_FORMAT_MASK_10BIT) &&
                !(m_VideoFormat & (VIDEO_FORMAT_MASK_YUV444 | VIDEO_FORMAT_MASK_YUV422));
            auto surface = QtVaapiVideoFrame::create(frame, (m_VideoFormat & VIDEO_FORMAT_MASK_10BIT) ? 10 : 8, error, allowP010);
            return QtVideoFrame::hardware(std::move(surface), std::move(feedback));
        }
        const auto format = getPreferredPixelFormat(m_VideoFormat);
        return QtVideoFrame::software(frame, format, error, std::move(feedback));
    }

    const bool m_Hardware;
    QtSoftwareVideoDevice::Ptr m_Device;
    const QString m_RenderNode;
    const QtDecodedVideoRendererFactory::OpenDelivery m_Open;
    QtDecodedVideoDelivery m_Delivery;
    AVBufferRef* m_HardwareDevice = nullptr;
    std::shared_ptr<PresentationStats> m_Stats = std::make_shared<PresentationStats>();
    int m_VideoFormat = 0;
    bool m_Identity = false;
    bool m_FrameRejected = false;
};
}

QtDecodedVideoRendererFactory::QtDecodedVideoRendererFactory(QtSoftwareVideoDevice::Ptr softwareDevice,
    QString vaapiRenderNode, OpenDelivery openDelivery)
    : m_SoftwareDevice(std::move(softwareDevice)), m_VaapiRenderNode(std::move(vaapiRenderNode)),
      m_OpenDelivery(std::move(openDelivery)) {}

IFFmpegRenderer* QtDecodedVideoRendererFactory::createRenderer(const AVCodecHWConfig* hardware)
{
    if (hardware && (hardware->device_type != AV_HWDEVICE_TYPE_VAAPI || hardware->pix_fmt != AV_PIX_FMT_VAAPI))
        return nullptr;
    return new Renderer(hardware != nullptr, m_SoftwareDevice, m_VaapiRenderNode, m_OpenDelivery);
}
