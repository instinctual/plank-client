#pragma once

#include "../video/ffmpeg-renderers/plvkframepool.h"
#include <libplacebo/dispatch.h>

#include <QString>

// Ubuntu Qt's software-decoded path. The decode allocator is the same cacheable
// imported pool as the existing Vulkan presenter. The GPU performs the planar
// -> RGB conversion; this class neither maps nor reads back video pixels.
//
// The rendering owner supplies/reuses an RGB10 output texture of the *decoded*
// size. Conversion does not resize: the Qt presenter applies the one shared
// source/destination transform. Output ownership and cross-API synchronization
// belong to that presenter, not to libplacebo's internal texture cache.
class QtSoftwareVideoConverter final
{
public:
    explicit QtSoftwareVideoConverter(pl_gpu gpu, std::shared_ptr<void> deviceLifetime = {});
    ~QtSoftwareVideoConverter();
    QtSoftwareVideoConverter(const QtSoftwareVideoConverter&) = delete;
    QtSoftwareVideoConverter& operator=(const QtSoftwareVideoConverter&) = delete;

    PlVkFramePool& framePool() { return m_Pool; }
    bool convert(const AVFrame* frame, AVPixelFormat expectedFormat, pl_tex output, QString& error);

    // RGB10_A2 storage matches GL_RGB10_A2 / Vulkan A2B10G10R10_UNORM_PACK32.
    // Do not choose a "similar" format that changes channels or precision.
    static pl_fmt outputFormat(pl_gpu gpu);

private:
    std::shared_ptr<void> m_DeviceLifetime;
    pl_gpu m_Gpu;
    PlVkFramePool m_Pool;
    pl_dispatch m_Dispatch = nullptr;
    pl_tex m_Planes[4]{};
};
