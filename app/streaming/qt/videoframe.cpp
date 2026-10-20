#include "videoframe.h"
extern "C" {
#include <libavutil/pixdesc.h>
}

QtVideoFrame::Ptr QtVideoFrame::hardware(QtVaapiVideoFrame::Ptr frame, Feedback feedback, Purpose purpose)
{
    if (!frame) return {};
    auto result = std::shared_ptr<QtVideoFrame>(new QtVideoFrame);
    result->m_Hardware = std::move(frame);
    result->m_Feedback = std::move(feedback);
    result->m_Purpose = purpose;
    return result;
}

QtVideoFrame::Ptr QtVideoFrame::software(const AVFrame* frame, AVPixelFormat expectedFormat, QString& error,
                                       Feedback feedback, Purpose purpose)
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
    result->m_Feedback = std::move(feedback);
    result->m_Purpose = purpose;
    return result;
}

void QtVideoFrame::presented(uint64_t renderMs) const
{
    if (!m_Presented.exchange(true) && m_Feedback) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_QueuedAt).count();
        m_Feedback(true, uint64_t(elapsed), renderMs);
    }
}

QtVideoFrame::~QtVideoFrame()
{
    if (!m_Presented.load() && m_Feedback) m_Feedback(false, 0, 0);
    av_frame_free(&m_Software);
}
