#include <QDebug>
#pragma once

#include "frameflowbuffer.h"
#include <chrono>

class ClientFrameFlowTrace {
public:
    enum Stage { Receive = 1, Decode = 2, Enqueue = 3, GpuWait = 4,
                 RenderBegin = 5, RenderEnd = 6, Drop = 7 };

    explicit ClientFrameFlowTrace(const char* lane) : m_Lane(lane), m_Buffer(Enabled) {}
    ~ClientFrameFlowTrace() { flush(); }

    static constexpr bool Enabled =
#ifdef PLANK_FRAME_FLOW_TRACE
        true;
#else
        false;
#endif

    static uint64_t nowNs() {
        if (!Enabled) return 0;
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void record(Stage stage, int64_t ptsUs = -1, int64_t frame = -1,
                int key = -1, uint64_t bytes = 0, int depth = -1,
                uint64_t durationNs = 0) {
        if (!Enabled) return;
        m_Buffer.record({nowNs(), ptsUs, frame, stage, key, bytes, depth, durationNs});
    }

    void flush() {
        if (!Enabled) return;
        const auto rows = m_Buffer.take();
        if (rows.empty()) return;
        qInfo("PLANK frame-flow begin lane=%s id=%llu rows=%llu capacity=%llu window_s=120 clock=steady-ns",
                    m_Lane, (unsigned long long)rows.front().timeNs, (unsigned long long)rows.size(),
                    (unsigned long long)FrameFlowBuffer::Capacity);
        qInfo("PLANK frame-flow columns=time_ns,pts_us,frame,stage,key,bytes,depth,duration_ns");
        for (const auto& row : rows) {
            qInfo("PLANK frame-flow row lane=%s id=%llu data=%llu,%lld,%lld,%d,%d,%llu,%d,%llu",
                        m_Lane, (unsigned long long)rows.front().timeNs,
                        (unsigned long long)row.timeNs, (long long)row.ptsUs,
                        (long long)row.frame, row.stage, row.key,
                        (unsigned long long)row.bytes, row.depth,
                        (unsigned long long)row.durationNs);
        }
        qInfo("PLANK frame-flow end lane=%s id=%llu",
                    m_Lane, (unsigned long long)rows.front().timeNs);
    }

private:
    const char* m_Lane;
    FrameFlowBuffer m_Buffer;
};
