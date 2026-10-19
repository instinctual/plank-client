#include "videoframe.h"
extern "C" {
#include <libavutil/pixdesc.h>
}

QtVideoFrame::Ptr QtVideoFrame::hardware(QtVaapiVideoFrame::Ptr frame)
{
    if (!frame) return {};
    auto result = std::shared_ptr<QtVideoFrame>(new QtVideoFrame);
    result->m_Hardware = std::move(frame);
    return result;
}

QtVideoFrame::Ptr QtVideoFrame::software(const AVFrame* frame, AVPixelFormat expectedFormat, QString& error)
{
    error.clear();
    const auto* descriptor = av_pix_fmt_desc_get(expectedFormat);
    if (!frame || !descriptor || frame->format != expectedFormat ||
            frame->width <= 0 || frame->height <= 0 || frame->width > 16384 || frame->height > 16384 ||
            frame->hw_frames_ctx || !frame->buf[0] || descriptor->nb_components != 3 ||
            !(descriptor->flags & AV_PIX_FMT_FLAG_PLANAR) ||
            (descriptor->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BE)) ||
            (descriptor->comp[0].depth != 8 && descriptor->comp[0].depth != 10)) {
        error = QStringLiteral("Qt video delivery requires a retained exact-format planar decoded frame");
        return {};
    }
    auto result = std::shared_ptr<QtVideoFrame>(new QtVideoFrame);
    result->m_Software = av_frame_clone(frame);
    result->m_Format = expectedFormat;
    if (!result->m_Software) {
        error = QStringLiteral("Could not retain the decoded Qt video frame"); return {};
    }
    return result;
}

QtVideoFrame::~QtVideoFrame() { av_frame_free(&m_Software); }
