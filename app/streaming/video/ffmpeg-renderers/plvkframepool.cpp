#include "plvkframepool.h"

// The C implementation is shared with the normal renderer (plvk_c.c).
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

extern "C" {
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <limits>
#include <new>
#include <numeric>

namespace {
bool alignUp(size_t value, size_t alignment, size_t* result)
{
    if (alignment == 0) return false;
    const size_t extra = (alignment - value % alignment) % alignment;
    if (value > std::numeric_limits<size_t>::max() - extra) return false;
    *result = value + extra;
    return true;
}

bool combineAlignment(size_t* alignment, size_t other)
{
    if (other == 0) return false;
    const size_t factor = other / std::gcd(*alignment, other);
    if (*alignment > static_cast<size_t>(INT_MAX) / factor) return false;
    *alignment *= factor;
    return true;
}
}

struct PlVkFramePool::State {
    pl_gpu gpu;
    std::shared_ptr<void> deviceLifetime;
    size_t hostAlignment;
    std::mutex allocationMutex;
    // Lookup by backing storage, not by dereferencing another allocator's opaque
    // pointer. Frame clones retain the same backing allocation.
    std::unordered_map<const uint8_t*, Allocation*> allocations;
};

struct PlVkFramePool::Allocation {
    std::shared_ptr<State> state;
    pl_buf buffer = nullptr;
    void* base = nullptr;
};

struct PlVkFramePool::Transfer {
    AVBufferRef* buffer = nullptr;
    std::atomic_bool complete{false};
    ~Transfer() { av_buffer_unref(&buffer); }
};

PlVkFramePool::PlVkFramePool(pl_gpu gpu, std::shared_ptr<void> deviceLifetime) :
    m_State(std::make_shared<State>())
{
    m_State->gpu = gpu;
    m_State->deviceLifetime = std::move(deviceLifetime);
    m_State->hostAlignment = gpu ? std::max<size_t>({
        gpu->limits.align_host_ptr, alignof(void*), 64,
    }) : 0;
}

PlVkFramePool::~PlVkFramePool()
{
    // This is shutdown only, after the caller stops the decoder/render worker.
    // Complete callbacks while the owner still holds the device, then retire the
    // pools. Outstanding external AVFrames can outlive the pools themselves.
    if (m_HasTransfers) pl_gpu_finish(m_State->gpu);
    m_Transfers.clear();
    for (auto& entry : m_Pools) av_buffer_pool_uninit(&entry.second);
}

bool PlVkFramePool::supported(pl_gpu gpu)
{
    return gpu && gpu->limits.thread_safe && gpu->limits.buf_transfer &&
        gpu->limits.callbacks && gpu->limits.align_host_ptr &&
        !(gpu->limits.align_host_ptr & (gpu->limits.align_host_ptr - 1)) &&
        (gpu->import_caps.buf & PL_HANDLE_HOST_PTR);
}

void PlVkFramePool::freeAllocation(void* opaque, uint8_t*)
{
    auto allocation = static_cast<Allocation*>(opaque);
    const auto state = allocation->state;
    {
        std::lock_guard<std::mutex> lock(state->allocationMutex);
        state->allocations.erase(static_cast<uint8_t*>(allocation->base));
    }
    pl_buf_destroy(state->gpu, &allocation->buffer);
    ::operator delete(allocation->base, std::align_val_t(state->hostAlignment));
    delete allocation;
}

AVBufferRef* PlVkFramePool::allocate(void* opaque, size_t size)
{
    const auto state = static_cast<PlVkFramePool*>(opaque)->m_State;
    if (size > state->gpu->limits.max_buf_size) return nullptr;
    auto allocation = new (std::nothrow) Allocation;
    if (!allocation) return nullptr;
    allocation->state = state;
    allocation->base = ::operator new(size, std::align_val_t(state->hostAlignment),
                                      std::nothrow);
    if (!allocation->base) {
        delete allocation;
        return nullptr;
    }

    pl_buf_params bufferParams = {};
    bufferParams.size = size;
    bufferParams.import_handle = PL_HANDLE_HOST_PTR;
    bufferParams.shared_mem.handle.ptr = allocation->base;
    bufferParams.shared_mem.size = size;
    allocation->buffer = pl_buf_create(state->gpu, &bufferParams);
    if (!allocation->buffer) {
        freeAllocation(allocation, nullptr);
        return nullptr;
    }

    try {
        std::lock_guard<std::mutex> lock(state->allocationMutex);
        state->allocations.emplace(static_cast<uint8_t*>(allocation->base), allocation);
    } catch (const std::bad_alloc&) {
        freeAllocation(allocation, nullptr);
        return nullptr;
    }
    AVBufferRef* buffer = av_buffer_create(static_cast<uint8_t*>(allocation->base),
                                          size, freeAllocation, allocation, 0);
    if (!buffer) freeAllocation(allocation, nullptr);
    return buffer;
}

AVBufferRef* PlVkFramePool::getBufferRef(size_t size)
{
    collectTransfers();
    std::lock_guard<std::mutex> lock(m_PoolMutex);
    try {
        AVBufferPool*& pool = m_Pools[size];
        if (!pool) {
            pool = av_buffer_pool_init2(size, this, allocate, nullptr);
            if (!pool) {
                m_Pools.erase(size);
                return nullptr;
            }
        }
        return av_buffer_pool_get(pool);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

int PlVkFramePool::getBuffer(AVCodecContext* context, AVFrame* frame, int flags)
{
    if (!context || !context->codec || !frame || !supported(m_State->gpu) ||
            frame->width <= 0 || frame->height <= 0 ||
            av_image_check_size(frame->width, frame->height, 0, nullptr) < 0)
        return AVERROR(EINVAL);
    // get_buffer2() receives an empty frame; do not overwrite live references.
    for (auto* buffer : frame->buf) if (buffer) return AVERROR(EINVAL);
    if (frame->extended_buf || frame->nb_extended_buf) return AVERROR(EINVAL);

    const auto format = static_cast<AVPixelFormat>(frame->format);
    pl_plane_data planes[4] = {};
    const int count = pl_plane_data_from_pixfmt(planes, nullptr, format);
    if (!(context->codec->capabilities & AV_CODEC_CAP_DR1) || count <= 0)
        return avcodec_default_get_buffer2(context, frame, flags);
    if (count > 4) return AVERROR(EINVAL);

    int width = frame->width, height = frame->height;
    int alignment[AV_NUM_DATA_POINTERS] = {};
    avcodec_align_dimensions2(context, &width, &height, alignment);
    int linesizes[4] = {};
    int result = av_image_fill_linesizes(linesizes, format, width);
    if (result < 0) return result;
    const auto& limits = m_State->gpu->limits;
    for (int plane = 0; plane < count; ++plane) {
        size_t pitchAlignment = std::max(1, alignment[plane]);
        size_t pitch;
        if (linesizes[plane] <= 0 ||
                !combineAlignment(&pitchAlignment, std::max<size_t>(1, limits.align_tex_xfer_pitch)) ||
                !combineAlignment(&pitchAlignment, std::max<size_t>(1, limits.align_tex_xfer_offset)) ||
                !combineAlignment(&pitchAlignment, planes[plane].pixel_stride) ||
                !alignUp(linesizes[plane], pitchAlignment, &pitch) || pitch > INT_MAX)
            return AVERROR(EINVAL);
        linesizes[plane] = static_cast<int>(pitch);
    }
    const ptrdiff_t strides[4] = { linesizes[0], linesizes[1], linesizes[2], linesizes[3] };
    size_t sizes[4] = {};
    result = av_image_fill_plane_sizes(sizes, format, height, strides);
    if (result < 0) return result;

    for (int plane = 0; plane < count; ++plane) {
        size_t size;
        if (!sizes[plane] || !alignUp(sizes[plane], m_State->hostAlignment, &size)) {
            av_frame_unref(frame);
            return AVERROR(EINVAL);
        }
        frame->buf[plane] = getBufferRef(size);
        if (!frame->buf[plane]) {
            av_frame_unref(frame);
            return AVERROR(ENOMEM);
        }
        frame->data[plane] = frame->buf[plane]->data;
        frame->linesize[plane] = linesizes[plane];
    }
    frame->extended_data = frame->data;
    return 0;
}

void PlVkFramePool::releaseTransfer(void* opaque)
{
    // libplacebo may run this inside pl_buf_create(), which is called with
    // FFmpeg's AVBufferPool mutex held. Never release a reference here: returning
    // it to the same pool would deadlock. Do not allocate or take locks either.
    static_cast<Transfer*>(opaque)->complete.store(true, std::memory_order_release);
}

void PlVkFramePool::collectTransfers()
{
    std::list<std::unique_ptr<Transfer>> completed;
    {
        std::lock_guard<std::mutex> lock(m_TransferMutex);
        for (auto it = m_Transfers.begin(); it != m_Transfers.end();) {
            auto current = it++;
            if ((*current)->complete.load(std::memory_order_acquire))
                completed.splice(completed.end(), m_Transfers, current);
        }
    }
    // Destruction/AVBuffer unref happens outside all our locks and GPU calls.
}

bool PlVkFramePool::upload(const AVFrame* frame, pl_frame* mappedFrame, pl_tex textures[4],
                         pl_fmt requiredPlaneFormat)
{
    collectTransfers();
    if (!frame || !mappedFrame || !textures || !supported(m_State->gpu) ||
            frame->width <= 0 || frame->height <= 0) return false;
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const auto* descriptor = av_pix_fmt_desc_get(format);
    if (!descriptor) return false;
    pl_plane_data planes[4] = {};
    pl_frame_from_avframe(mappedFrame, frame);
    mappedFrame->user_data = nullptr;
    const int count = pl_plane_data_from_pixfmt(planes, &mappedFrame->repr.bits, format);
    if (count <= 0 || count > 4) return false;
    if (requiredPlaneFormat) {
        const auto fmt = requiredPlaneFormat;
        if (fmt->type != PL_FMT_UNORM || fmt->opaque || fmt->emulated ||
                fmt->num_components != 1 || fmt->sample_order[0] != 0 ||
                (fmt->component_depth[0] != 8 && fmt->component_depth[0] != 16) ||
                fmt->host_bits[0] != fmt->component_depth[0] ||
                !(fmt->caps & PL_FMT_CAP_SAMPLEABLE)) return false;
        for (int p = 0; p < count; ++p) {
            const auto& data = planes[p];
            if (data.type != fmt->type || data.swapped || data.component_pad[0] ||
                    data.component_size[0] != fmt->host_bits[0] || data.component_size[1] ||
                    data.pixel_stride != fmt->texel_size) return false;
        }
    }

    AVBufferRef* buffers[4] = {};
    // Validate every plane before submitting any GPU work.
    for (int plane = 0; plane < count; ++plane) {
        buffers[plane] = av_frame_get_plane_buffer(const_cast<AVFrame*>(frame), plane);
        if (!buffers[plane] || !frame->data[plane]) return false;
        {
            std::lock_guard<std::mutex> lock(m_State->allocationMutex);
            const auto entry = m_State->allocations.find(buffers[plane]->data);
            if (entry == m_State->allocations.end()) return false;
            planes[plane].buf = entry->second->buffer;
        }
        const bool chroma = plane == 1 || plane == 2;
        auto& data = planes[plane];
        data.width = AV_CEIL_RSHIFT(frame->width, chroma ? descriptor->log2_chroma_w : 0);
        data.height = AV_CEIL_RSHIFT(frame->height, chroma ? descriptor->log2_chroma_h : 0);
        const int64_t stride = frame->linesize[plane];
        data.row_stride = static_cast<size_t>(stride < 0 ? -stride : stride);
        const auto base = reinterpret_cast<uintptr_t>(buffers[plane]->data);
        const auto start = reinterpret_cast<uintptr_t>(frame->data[plane]);
        if (start < base || start - base >= buffers[plane]->size ||
                data.row_stride < static_cast<size_t>(data.width) * data.pixel_stride)
            return false;
        data.buf_offset = start - base;
        const size_t rows = data.height - 1;
        if (rows && data.row_stride > std::numeric_limits<size_t>::max() / rows) return false;
        const size_t rowSpan = data.row_stride * rows;
        if (stride < 0) {
            if (data.buf_offset < rowSpan) return false;
            data.buf_offset -= rowSpan;
        }
        const size_t remaining = buffers[plane]->size - data.buf_offset;
        if (rowSpan > remaining || static_cast<size_t>(data.width) * data.pixel_stride > remaining - rowSpan ||
                data.buf_offset % 4 || data.buf_offset % data.pixel_stride)
            return false;
        mappedFrame->planes[plane].flipped = stride < 0;
    }

    for (int plane = 0; plane < count; ++plane) {
        auto& data = planes[plane];
        auto transfer = std::unique_ptr<Transfer>(new (std::nothrow) Transfer);
        if (!transfer) return false;
        transfer->buffer = av_buffer_ref(buffers[plane]);
        if (!transfer->buffer) return false;
        data.priv = transfer.get();
        try {
            std::lock_guard<std::mutex> lock(m_TransferMutex);
            m_Transfers.push_back(std::move(transfer));
        } catch (const std::bad_alloc&) {
            return false;
        }
        data.callback = releaseTransfer;
        m_HasTransfers = true;
        bool uploaded;
        if (requiredPlaneFormat) {
            pl_tex_params params{};
            params.w = data.width; params.h = data.height;
            params.format = requiredPlaneFormat;
            params.sampleable = params.host_writable = true;
            uploaded = pl_tex_recreate(m_State->gpu, &textures[plane], &params);
            if (uploaded) {
                auto& output = mappedFrame->planes[plane];
                output.components = 1;
                for (int& mapping : output.component_mapping) mapping = -1;
                output.component_mapping[0] = data.component_map[0];
                pl_tex_transfer_params transfer{};
                transfer.tex = textures[plane];
                transfer.rc.x1 = data.width; transfer.rc.y1 = data.height; transfer.rc.z1 = 1;
                transfer.row_pitch = data.row_stride;
                transfer.buf = data.buf; transfer.buf_offset = data.buf_offset;
                transfer.callback = data.callback; transfer.priv = data.priv;
                uploaded = pl_tex_upload(m_State->gpu, &transfer);
            }
        } else uploaded = pl_upload_plane(m_State->gpu, &mappedFrame->planes[plane], &textures[plane], &data);
        if (!uploaded) {
            releaseTransfer(data.priv);
            collectTransfers();
            return false;
        }
        mappedFrame->planes[plane].texture = textures[plane];
    }
    return true;
}
