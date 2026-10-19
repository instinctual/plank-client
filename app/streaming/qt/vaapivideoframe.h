#pragma once

#include "../video/packedvideo.h"

#include <QSize>
#include <QString>
#include <memory>
#include <va/va_drmcommon.h>

struct AVFrame;

// Immutable decoded-surface lease. Export/synchronization happens on the
// decoder worker, never in the Qt GUI or render loop. No pixel mapping/copy.
// Retaining the AVFrame is essential: exported FDs alone do not stop the decoder
// from reusing a surface while the presentation GPU still reads it.
class QtVaapiVideoFrame final
{
public:
    using Ptr = std::shared_ptr<const QtVaapiVideoFrame>;
    static Ptr create(const AVFrame* frame, int expectedBits, QString& error, bool allowP010 = false);
    ~QtVaapiVideoFrame();
    QtVaapiVideoFrame(const QtVaapiVideoFrame&) = delete;
    QtVaapiVideoFrame& operator=(const QtVaapiVideoFrame&) = delete;

    QSize size() const { return m_Size; }
    PlankPackedVideo::Mapping mapping() const { return m_Mapping; }
    bool planar() const { return m_Planar; }
    uint32_t importFormat(unsigned plane = 0) const;
    QSize planeSize(unsigned plane = 0) const;
    const VADRMPRIMESurfaceDescriptor& descriptor() const { return m_Descriptor; }

    // Also exercised independently with malformed/export-layout fixtures.
    static bool validate(const VADRMPRIMESurfaceDescriptor& descriptor,
                         const QSize& size, int bits, bool identity, bool bt709,
                         bool fullRange, bool allowP010 = false);

private:
    QtVaapiVideoFrame() = default;
    AVFrame* m_Frame = nullptr;
    VADRMPRIMESurfaceDescriptor m_Descriptor = {};
    QSize m_Size;
    PlankPackedVideo::Mapping m_Mapping = PlankPackedVideo::Mapping::Unsupported;
    bool m_Planar = false;
};
