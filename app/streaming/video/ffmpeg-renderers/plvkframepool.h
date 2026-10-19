#pragma once

#include <libplacebo/renderer.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <memory>
#include <list>
#include <mutex>
#include <unordered_map>

// Cacheable FFmpeg reference frames imported into Vulkan, independent of the
// window system. Decoding writes directly into the imported allocations; plane
// uploads do not make a second CPU copy. getBuffer() is decoder-thread safe.
// upload() and destruction belong to the rendering owner, after decoding stops.
class PlVkFramePool final
{
public:
    // A Qt rendering device can supply a lifetime token so outstanding AVFrames
    // keep its GPU alive across session teardown. With a borrowed GPU, the caller
    // must release all frames before destroying that GPU (the SDL renderer does).
    explicit PlVkFramePool(pl_gpu gpu, std::shared_ptr<void> deviceLifetime = {});
    ~PlVkFramePool();
    PlVkFramePool(const PlVkFramePool&) = delete;
    PlVkFramePool& operator=(const PlVkFramePool&) = delete;

    static bool supported(pl_gpu gpu);
    int getBuffer(AVCodecContext* context, AVFrame* frame, int flags);

    // Textures belong to the renderer. A transfer retains its source AVBuffer
    // until the GPU completes the upload, preventing premature decoder reuse.
    // No per-frame GPU wait is introduced. Returns false for foreign frames,
    // malformed plane bounds, or an unsupported GPU/format.
    // A caller doing explicit code-value conversion can require exact R8/R16
    // storage instead of libplacebo's automatic compatible-format selection.
    bool upload(const AVFrame* frame, pl_frame* mappedFrame, pl_tex textures[4],
                pl_fmt requiredPlaneFormat = nullptr);

private:
    struct State;
    struct Allocation;
    struct Transfer;
    static AVBufferRef* allocate(void* opaque, size_t size);
    static void freeAllocation(void* opaque, uint8_t* data);
    static void releaseTransfer(void* opaque);
    void collectTransfers();
    AVBufferRef* getBufferRef(size_t size);

    std::shared_ptr<State> m_State;
    std::mutex m_PoolMutex;
    std::unordered_map<size_t, AVBufferPool*> m_Pools;
    std::mutex m_TransferMutex;
    std::list<std::unique_ptr<Transfer>> m_Transfers;
    bool m_HasTransfers = false;
};
