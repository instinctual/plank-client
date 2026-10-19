#include "vaapivideoframe.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <algorithm>
#include <climits>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>

QtVaapiVideoFrame::~QtVaapiVideoFrame()
{
    for (uint32_t i = 0; i < std::min(m_Descriptor.num_objects, uint32_t(4)); ++i)
        if (m_Descriptor.objects[i].fd >= 0) close(m_Descriptor.objects[i].fd);
    av_frame_free(&m_Frame);
}

bool QtVaapiVideoFrame::validate(const VADRMPRIMESurfaceDescriptor& d,
                                const QSize& size, int bits, bool identity,
                                bool bt709, bool fullRange)
{
    // Qualified packed path only. Do not silently reinterpret another format,
    // bit depth, range, compressed auxiliary plane or chroma arrangement.
    if (size.width() <= 0 || size.height() <= 0 || size.width() > 16384 || size.height() > 16384 ||
            d.width < uint32_t(size.width()) || d.height < uint32_t(size.height()) ||
            d.num_objects != 1 || d.num_layers != 1 || d.layers[0].num_planes != 1 ||
            d.layers[0].object_index[0] != 0 || d.objects[0].fd < 0)
        return false;
    const auto& plane = d.layers[0];
    const auto& object = d.objects[0];
    if (PlankPackedVideo::mapping(plane.drm_format, bits, identity, bt709, fullRange) ==
            PlankPackedVideo::Mapping::Unsupported)
        return false;
    // All currently accepted storage layouts are four bytes per pixel. With
    // tiled/modifier layouts, size is not a linear pitch*height footprint.
    if (plane.pitch[0] < uint32_t(size.width()) * 4 || plane.pitch[0] > INT_MAX ||
            plane.offset[0] > INT_MAX || object.size <= plane.offset[0])
        return false;
    if (object.drm_format_modifier == DRM_FORMAT_MOD_LINEAR) {
        const uint64_t end = uint64_t(plane.offset[0]) +
            uint64_t(plane.pitch[0]) * (size.height() - 1) + uint64_t(size.width()) * 4;
        if (end > object.size) return false;
    }
    return true;
}

uint32_t QtVaapiVideoFrame::importFormat() const
{
    return PlankPackedVideo::importFormat(m_Descriptor.layers[0].drm_format,
        m_Mapping == PlankPackedVideo::Mapping::Identity8 || m_Mapping == PlankPackedVideo::Mapping::Identity10,
        m_Mapping == PlankPackedVideo::Mapping::Bt709Full10 ||
        m_Mapping == PlankPackedVideo::Mapping::Bt709Limited10);
}

QtVaapiVideoFrame::Ptr QtVaapiVideoFrame::create(const AVFrame* frame, int expectedBits, QString& error)
{
    error.clear();
    if (!frame || frame->format != AV_PIX_FMT_VAAPI || !frame->hw_frames_ctx ||
            (expectedBits != 8 && expectedBits != 10)) {
        error = QStringLiteral("Qt video requires an exact-format VA-API frame");
        return {};
    }
    auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    if (!frames || !frames->device_ctx || frames->device_ctx->type != AV_HWDEVICE_TYPE_VAAPI ||
            !frames->device_ctx->hwctx) {
        error = QStringLiteral("Qt video received an invalid VA-API device");
        return {};
    }
    const bool identity = frame->colorspace == AVCOL_SPC_RGB;
    const bool bt709 = frame->colorspace == AVCOL_SPC_BT709;
    const bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;
    if ((!identity && !bt709) || (!fullRange && (identity || frame->color_range != AVCOL_RANGE_MPEG))) {
        error = QStringLiteral("Qt packed video requires full-range identity RGB or explicit full/limited BT.709");
        return {};
    }
    auto lease = std::shared_ptr<QtVaapiVideoFrame>(new QtVaapiVideoFrame);
    lease->m_Frame = av_frame_clone(frame);
    if (!lease->m_Frame) { error = QStringLiteral("Could not retain decoded video surface"); return {}; }
    lease->m_Size = {frame->width, frame->height};
    auto* device = reinterpret_cast<AVVAAPIDeviceContext*>(frames->device_ctx->hwctx);
    const auto surface = VASurfaceID(uintptr_t(frame->data[3]));
    // Bounded wait; unlike vaSyncSurface(), a stalled GPU cannot leave an
    // indefinitely blocked decoder/export worker during teardown.
    const VAStatus synced = vaSyncSurface2(device->display, surface, 100000000ULL);
    if (synced != VA_STATUS_SUCCESS) {
        error = QStringLiteral("VA-API surface was not ready within 100 ms (%1)").arg(int(synced));
        return {};
    }
    const VAStatus exported = vaExportSurfaceHandle(device->display, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &lease->m_Descriptor);
    if (exported != VA_STATUS_SUCCESS) {
        // A failed export has no defined output descriptor or owned FDs.
        lease->m_Descriptor = {};
        error = QStringLiteral("VA-API DMA-BUF export failed (%1)").arg(int(exported));
        return {};
    }
    if (!validate(lease->m_Descriptor, lease->m_Size, expectedBits, identity, bt709, fullRange)) {
        error = QStringLiteral("VA-API surface does not match the selected packed video format");
        return {};
    }
    const int fd = lease->m_Descriptor.objects[0].fd;
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        error = QStringLiteral("Could not secure the exported video descriptor");
        return {};
    }
    lease->m_Mapping = PlankPackedVideo::mapping(lease->m_Descriptor.layers[0].drm_format,
                                              expectedBits, identity, bt709, fullRange);
    return lease;
}
