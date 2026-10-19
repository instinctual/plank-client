#include "softwarevideoconverter.h"

#include <drm_fourcc.h>
extern "C" {
#include <libavutil/pixdesc.h>
}

QtSoftwareVideoConverter::QtSoftwareVideoConverter(pl_gpu gpu, std::shared_ptr<void> deviceLifetime)
    : m_DeviceLifetime(std::move(deviceLifetime)), m_Gpu(gpu), m_Pool(gpu, m_DeviceLifetime)
{
    if (PlVkFramePool::supported(gpu)) m_Renderer = pl_renderer_create(nullptr, gpu);
}

QtSoftwareVideoConverter::~QtSoftwareVideoConverter()
{
    // Decoder/render callers must have stopped first. libplacebo retires its
    // GPU objects safely; the pool retains asynchronous upload source leases.
    pl_renderer_destroy(&m_Renderer);
    for (auto& plane : m_Planes) pl_tex_destroy(m_Gpu, &plane);
}

pl_fmt QtSoftwareVideoConverter::outputFormat(pl_gpu gpu)
{
    const auto format = gpu ? pl_find_fourcc(gpu, DRM_FORMAT_ABGR2101010) : nullptr;
    const auto caps = PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE;
    if (!format || format->type != PL_FMT_UNORM || format->num_components != 4 ||
            (format->caps & caps) != caps || format->opaque || format->emulated ||
            format->texel_size != 4) return nullptr;
    for (int i = 0; i < 4; ++i) {
        if (format->component_depth[i] != (i == 3 ? 2 : 10) ||
                format->host_bits[i] != (i == 3 ? 2 : 10) || format->sample_order[i] != i)
            return nullptr;
    }
    return format;
}

bool QtSoftwareVideoConverter::convert(const AVFrame* frame, AVPixelFormat expectedFormat,
                                      pl_tex output, QString& error)
{
    error.clear();
    const auto format = outputFormat(m_Gpu);
    const auto* descriptor = av_pix_fmt_desc_get(expectedFormat);
    if (!m_Renderer || !frame || !output || !format || !descriptor ||
            frame->format != expectedFormat || frame->width <= 0 || frame->height <= 0 ||
            frame->width > 16384 || frame->height > 16384 || descriptor->nb_components != 3 ||
            !(descriptor->flags & AV_PIX_FMT_FLAG_PLANAR) ||
            (descriptor->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BE)) ||
            (descriptor->comp[0].depth != 8 && descriptor->comp[0].depth != 10) ||
            descriptor->comp[1].depth != descriptor->comp[0].depth ||
            descriptor->comp[2].depth != descriptor->comp[0].depth ||
            output->params.format != format || !output->params.renderable ||
            output->params.w != frame->width || output->params.h != frame->height || output->params.d) {
        error = QStringLiteral("Qt software video requires the exact decoded format and a same-size RGB10 target");
        return false;
    }
    const bool rgb = descriptor->flags & AV_PIX_FMT_FLAG_RGB;
    if (frame->color_range != AVCOL_RANGE_JPEG ||
            (rgb ? frame->colorspace != AVCOL_SPC_RGB : frame->colorspace != AVCOL_SPC_BT709) ||
            (frame->color_primaries != AVCOL_PRI_UNSPECIFIED && frame->color_primaries != AVCOL_PRI_BT709) ||
            (frame->color_trc != AVCOL_TRC_UNSPECIFIED && frame->color_trc != AVCOL_TRC_BT709 &&
             frame->color_trc != AVCOL_TRC_IEC61966_2_1)) {
        error = QStringLiteral("Qt software video requires full-range identity RGB or BT.709 SDR");
        return false;
    }
    pl_frame source{};
    if (!m_Pool.upload(frame, &source, m_Planes)) {
        error = QStringLiteral("Qt software video could not import the decoder's pooled planes");
        return false;
    }
    pl_frame target{};
    target.num_planes = 1;
    target.planes[0].texture = output;
    target.planes[0].components = 4;
    for (int i = 0; i < 4; ++i) target.planes[0].component_mapping[i] = i;
    target.repr = pl_color_repr_rgb;
    target.repr.bits.sample_depth = target.repr.bits.color_depth = 10;
    // Preserve code values: no transfer-function, gamut, range, HDR, or display
    // color-management conversion. Subsampled YCbCr still uses its BT.709 matrix.
    target.color = source.color;
    target.crop = {0, 0, float(frame->width), float(frame->height)};
    auto params = pl_render_fast_params;
    params.dither_params = nullptr;
    params.error_diffusion = nullptr;
    params.color_map_params = nullptr;
    if (!pl_render_image(m_Renderer, &source, &target, &params) ||
            pl_renderer_get_errors(m_Renderer).errors != PL_RENDER_ERR_NONE) {
        error = QStringLiteral("Qt software video GPU conversion failed");
        return false;
    }
    return true;
}
