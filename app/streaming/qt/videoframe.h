#pragma once

#include "vaapivideoframe.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

// Immutable retained decode result. One payload type crosses the coalescing
// mailbox for either decoder path; no queued pixel copies or raw AVFrame borrows.
class QtVideoFrame final
{
public:
    using Ptr = std::shared_ptr<const QtVideoFrame>;
    static Ptr hardware(QtVaapiVideoFrame::Ptr frame);
    static Ptr software(const AVFrame* frame, AVPixelFormat expectedFormat, QString& error);
    ~QtVideoFrame();
    QtVideoFrame(const QtVideoFrame&) = delete;
    QtVideoFrame& operator=(const QtVideoFrame&) = delete;

    QSize size() const { return m_Hardware ? m_Hardware->size() : QSize(m_Software->width, m_Software->height); }
    QtVaapiVideoFrame::Ptr hardwareFrame() const { return m_Hardware; }
    const AVFrame* softwareFrame() const { return m_Software; }
    AVPixelFormat softwareFormat() const { return m_Format; }

private:
    QtVideoFrame() = default;
    QtVaapiVideoFrame::Ptr m_Hardware;
    AVFrame* m_Software = nullptr;
    AVPixelFormat m_Format = AV_PIX_FMT_NONE;
};
