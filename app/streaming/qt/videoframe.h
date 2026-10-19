#pragma once

#include "vaapivideoframe.h"
#include <atomic>
#include <chrono>
#include <functional>
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
    // At most one completion, from the primary output after frameSwapped.
    // Destruction without presentation reports replacement/discard. The
    // callback must be thread-safe and must not retain this frame.
    using Feedback = std::function<void(bool presented, uint64_t queuedMs, uint64_t renderMs)>;
    static Ptr hardware(QtVaapiVideoFrame::Ptr frame, Feedback feedback = {});
    static Ptr software(const AVFrame* frame, AVPixelFormat expectedFormat, QString& error,
                        Feedback feedback = {});
    ~QtVideoFrame();
    QtVideoFrame(const QtVideoFrame&) = delete;
    QtVideoFrame& operator=(const QtVideoFrame&) = delete;

    QSize size() const { return m_Hardware ? m_Hardware->size() : QSize(m_Software->width, m_Software->height); }
    QtVaapiVideoFrame::Ptr hardwareFrame() const { return m_Hardware; }
    const AVFrame* softwareFrame() const { return m_Software; }
    AVPixelFormat softwareFormat() const { return m_Format; }
    void presented(uint64_t renderMs) const;

private:
    QtVideoFrame() = default;
    QtVaapiVideoFrame::Ptr m_Hardware;
    AVFrame* m_Software = nullptr;
    AVPixelFormat m_Format = AV_PIX_FMT_NONE;
    Feedback m_Feedback;
    const std::chrono::steady_clock::time_point m_QueuedAt = std::chrono::steady_clock::now();
    mutable std::atomic<bool> m_Presented{false};
};
