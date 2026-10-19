#include "softwarevideoconverter.h"

#include <drm_fourcc.h>
#include <libplacebo/shaders/custom.h>
extern "C" {
#include <libavutil/pixdesc.h>
}

QtSoftwareVideoConverter::QtSoftwareVideoConverter(pl_gpu gpu, std::shared_ptr<void> deviceLifetime)
    : m_DeviceLifetime(std::move(deviceLifetime)), m_Gpu(gpu), m_Pool(gpu, m_DeviceLifetime)
{
    if (PlVkFramePool::supported(gpu)) m_Dispatch = pl_dispatch_create(nullptr, gpu);
}

QtSoftwareVideoConverter::~QtSoftwareVideoConverter()
{
    // Decoder/render callers must have stopped first. libplacebo retires its
    // GPU objects safely; the pool retains asynchronous upload source leases.
    pl_dispatch_destroy(&m_Dispatch);
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
    if (!m_Dispatch || !frame || !output || !format || !descriptor ||
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
    if (frame->crop_left || frame->crop_right || frame->crop_top || frame->crop_bottom ||
            (frame->flags & AV_FRAME_FLAG_INTERLACED) ||
            av_frame_get_side_data(frame, AV_FRAME_DATA_ICC_PROFILE) ||
            av_frame_get_side_data(frame, AV_FRAME_DATA_FILM_GRAIN_PARAMS)) {
        error = QStringLiteral("Qt software video does not accept implicit cropping, interlace, ICC or film-grain processing");
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
    // One explicit GPU pass: preserve RGB codes or apply the same full-range
    // BT.709 matrix as our packed hardware path. A general-purpose color
    // renderer infers YCbCr's neutral point from the *storage* depth (16 for
    // 10-bit planes), which shifts gray. The decoded precision is authoritative.
    // No transfer functions, color management, dithering or extra resize pass.
    const int depth = descriptor->comp[0].depth;
    const int maximum = (1 << depth) - 1;
    pl_shader_desc bindings[3]{};
    const char* names[] = {"plane0", "plane1", "plane2"};
    QString body = QStringLiteral("vec3 samples = vec3(0.0);\n");
    if (source.num_planes != 3 || source.repr.bits.bit_shift) {
        error = QStringLiteral("Qt software video requires three unshifted planes");
        return false;
    }
    for (int p = 0; p < 3; ++p) {
        const auto& plane = source.planes[p];
        const auto& texture = plane.texture->params;
        const auto pf = texture.format;
        if (plane.components != 1 || pf->num_components != 1 || pf->type != PL_FMT_UNORM ||
                pf->component_depth[0] != (depth == 8 ? 8 : 16) ||
                !(pf->caps & PL_FMT_CAP_LINEAR) || plane.component_mapping[0] < 0 ||
                plane.component_mapping[0] > 2) {
            error = QStringLiteral("Qt software video plane %1 is not exact R8/R16 UNORM: %2, components=%3/%4 type=%5 depth=%6 linear=%7 mapping=%8")
                        .arg(p).arg(pf->name).arg(plane.components).arg(pf->num_components)
                        .arg(int(pf->type)).arg(pf->component_depth[0])
                        .arg(bool(pf->caps & PL_FMT_CAP_LINEAR)).arg(plane.component_mapping[0]);
            return false;
        }
        bindings[p].desc.name = names[p];
        bindings[p].desc.type = PL_DESC_SAMPLED_TEX;
        bindings[p].binding.object = plane.texture;
        bindings[p].binding.address_mode = PL_TEX_ADDRESS_CLAMP;
        bindings[p].binding.sample_mode = PL_TEX_SAMPLE_LINEAR;
        const int subX = p ? 1 << descriptor->log2_chroma_w : 1;
        const int subY = p ? 1 << descriptor->log2_chroma_h : 1;
        // Chroma siting is expressed in reference pixels. Using the padded
        // plane extent (not frame.width) also handles odd frame dimensions.
        body += QStringLiteral("vec2 uv%1 = (gl_FragCoord.xy - vec2(%2, %3)) / vec2(%4.0, %5.0);\n")
                    .arg(p).arg(double(plane.shift_x), 0, 'f', 8).arg(double(plane.shift_y), 0, 'f', 8)
                    .arg(texture.w * subX).arg(texture.h * subY);
        if (plane.flipped) body += QStringLiteral("uv%1.y = 1.0 - uv%1.y;\n").arg(p);
        body += QStringLiteral("samples[%1] = texture(%2, uv%3).r * (%4.0 / %5.0);\n")
                    .arg(plane.component_mapping[0]).arg(names[p]).arg(p)
                    .arg((1 << pf->component_depth[0]) - 1).arg(maximum);
    }
    if (rgb) body += QStringLiteral("color = vec4(samples, 1.0);\n");
    else body += QStringLiteral(
                "float y = samples.x;\n"
                "float u = samples.y - (%1.0 / %2.0);\n"
                "float v = samples.z - (%1.0 / %2.0);\n"
                "color = vec4(y + 1.5748 * v, y - 0.1873242729 * u - 0.4681242729 * v, y + 1.8556 * u, 1.0);\n")
                    .arg(1 << (depth - 1)).arg(maximum);
    const auto text = body.toUtf8();
    pl_custom_shader custom{};
    custom.description = "PLANK exact planar to RGB10";
    custom.body = text.constData();
    custom.output = PL_SHADER_SIG_COLOR;
    custom.descriptors = bindings;
    custom.num_descriptors = 3;
    custom.output_w = frame->width;
    custom.output_h = frame->height;
    pl_dispatch_reset_frame(m_Dispatch);
    pl_shader shader = pl_dispatch_begin(m_Dispatch);
    if (!shader || !pl_shader_custom(shader, &custom)) {
        pl_dispatch_abort(m_Dispatch, &shader);
        error = QStringLiteral("Qt software video GPU shader could not be created");
        return false;
    }
    pl_dispatch_params params{};
    params.shader = &shader;
    params.target = output;
    if (!pl_dispatch_finish(m_Dispatch, &params)) {
        error = QStringLiteral("Qt software video GPU conversion failed");
        return false;
    }
    return true;
}
