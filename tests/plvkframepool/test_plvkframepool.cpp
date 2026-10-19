#include "plvkframepool.h"
#include "ffmpeg-testframes.h"

#include <libplacebo/vulkan.h>
#include <libplacebo/utils/upload.h>
extern "C" {
#include <libavutil/imgutils.h>
}
#include <QtTest>
#include <atomic>
#include <climits>
#include <thread>
#include <vector>

namespace {
bool realGpu = false;
std::atomic_int allocations{0}, frees{0};
int failAllocation = 0, failUpload = 0, uploads = 0, finishes = 0;
struct Transfer { void (*callback)(void*); void* opaque; pl_plane_data data; };
std::vector<Transfer> transfers;

void finishTransfers()
{
    auto pending = std::move(transfers);
    transfers.clear();
    for (const auto& t : pending) t.callback(t.opaque);
}

struct Fixture {
    pl_gpu_t gpu{};
    AVCodecContext* codec = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    AVFrame* frame = av_frame_alloc();
    Fixture()
    {
        gpu.limits.thread_safe = gpu.limits.callbacks = gpu.limits.buf_transfer = true;
        gpu.limits.align_host_ptr = 4096;
        gpu.limits.max_buf_size = INT_MAX;
        gpu.limits.align_tex_xfer_pitch = gpu.limits.align_tex_xfer_offset = 4;
        gpu.import_caps.buf = PL_HANDLE_HOST_PTR;
        codec->pix_fmt = AV_PIX_FMT_GBRP10LE;
        prepare();
    }
    void prepare()
    {
        frame->format = codec->pix_fmt;
        frame->width = 256; frame->height = 64;
        frame->color_range = AVCOL_RANGE_JPEG;
        frame->colorspace = AVCOL_SPC_RGB;
    }
    ~Fixture() { av_frame_free(&frame); avcodec_free_context(&codec); }
};
struct VulkanDevice {
    pl_vulkan vk = nullptr;
    ~VulkanDevice() { pl_vulkan_destroy(&vk); }
};

int getBuffer(AVCodecContext* context, AVFrame* frame, int flags)
{
    return static_cast<PlVkFramePool*>(context->opaque)->getBuffer(context, frame, flags);
}

AVFrame* decode(const QByteArray& sample, AVCodecID codecId, PlVkFramePool* pool)
{
    const AVCodec* decoder = avcodec_find_decoder(codecId);
    AVCodecContext* context = avcodec_alloc_context3(decoder);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (context && packet && frame) {
        context->thread_count = 4;
        if (pool) { context->opaque = pool; context->get_buffer2 = getBuffer; }
        if (avcodec_open2(context, decoder, nullptr) >= 0 &&
                av_new_packet(packet, sample.size()) >= 0) {
            memcpy(packet->data, sample.constData(), sample.size());
            if (avcodec_send_packet(context, packet) >= 0) {
                avcodec_send_packet(context, nullptr);
                if (avcodec_receive_frame(context, frame) < 0) av_frame_free(&frame);
            } else av_frame_free(&frame);
        } else av_frame_free(&frame);
    } else av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&context);
    return frame;
}

QByteArray visibleBytes(const AVFrame* frame)
{
    auto format = static_cast<AVPixelFormat>(frame->format);
    QByteArray bytes(av_image_get_buffer_size(format, frame->width, frame->height, 1), '\0');
    if (av_image_copy_to_buffer(reinterpret_cast<uint8_t*>(bytes.data()), bytes.size(),
                                frame->data, frame->linesize, format,
                                frame->width, frame->height, 1) < 0) return {};
    return bytes;
}
}

extern "C" {
pl_buf __real_pl_buf_create(pl_gpu, const pl_buf_params*);
void __real_pl_buf_destroy(pl_gpu, pl_buf*);
bool __real_pl_upload_plane(pl_gpu, pl_plane*, pl_tex*, const pl_plane_data*);
void __real_pl_gpu_finish(pl_gpu);

pl_buf __wrap_pl_buf_create(pl_gpu gpu, const pl_buf_params* params)
{
    const int count = ++allocations;
    if (realGpu) return __real_pl_buf_create(gpu, params);
    if (count == failAllocation) return nullptr;
    if (params->import_handle != PL_HANDLE_HOST_PTR ||
            reinterpret_cast<uintptr_t>(params->shared_mem.handle.ptr) % gpu->limits.align_host_ptr)
        return nullptr;
    auto buffer = new pl_buf_t{};
    buffer->params = *params;
    return buffer;
}
void __wrap_pl_buf_destroy(pl_gpu gpu, pl_buf* buffer)
{
    if (!*buffer) return;
    ++frees;
    if (realGpu) { __real_pl_buf_destroy(gpu, buffer); return; }
    delete *buffer;
    *buffer = nullptr;
}
bool __wrap_pl_upload_plane(pl_gpu gpu, pl_plane* plane, pl_tex* texture, const pl_plane_data* data)
{
    ++uploads;
    if (realGpu) return __real_pl_upload_plane(gpu, plane, texture, data);
    if (uploads == failUpload) return false;
    if (!data->buf || data->pixels || !data->callback) return false;
    transfers.push_back({data->callback, data->priv, *data});
    return true;
}
void __wrap_pl_gpu_finish(pl_gpu gpu)
{
    ++finishes;
    if (realGpu) { __real_pl_gpu_finish(gpu); return; }
    finishTransfers();
}
}

class FramePoolTest : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        realGpu = false;
        allocations = frees = 0;
        uploads = finishes = failAllocation = failUpload = 0;
        QVERIFY(transfers.empty());
    }
    void cleanup()
    {
        QVERIFY(transfers.empty());
    }
    void capabilities()
    {
        Fixture f;
        QVERIFY(PlVkFramePool::supported(&f.gpu));
        f.gpu.limits.callbacks = false;
        QVERIFY(!PlVkFramePool::supported(&f.gpu));
        f.gpu.limits.callbacks = true;
        f.gpu.limits.align_host_ptr = 3072;
        QVERIFY(!PlVkFramePool::supported(&f.gpu));
        QVERIFY(!PlVkFramePool::supported(nullptr));
    }
    void reuseAndReferences()
    {
        Fixture f;
        PlVkFramePool pool(&f.gpu);
        QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
        auto* clone = av_frame_clone(f.frame);
        QVERIFY(clone);
        QCOMPARE(allocations.load(), 3);
        av_frame_unref(f.frame); f.prepare();
        QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
        QCOMPARE(allocations.load(), 6);
        av_frame_free(&clone);
        av_frame_unref(f.frame); f.prepare();
        QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
        QCOMPARE(allocations.load(), 6);
    }
    void transfersRetainBackingMemory()
    {
        Fixture f;
        {
            PlVkFramePool pool(&f.gpu);
            QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
            pl_frame mapped{}; pl_tex textures[4]{};
            QVERIFY(pool.upload(f.frame, &mapped, textures));
            QCOMPARE(transfers.size(), size_t(3));
            QCOMPARE(finishes, 0);
            av_frame_unref(f.frame); f.prepare();
            QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
            QCOMPARE(allocations.load(), 6); // GPU still owns the first three
            finishTransfers();
            av_frame_unref(f.frame); f.prepare();
            QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
            QCOMPARE(allocations.load(), 6);
            QVERIFY(pool.upload(f.frame, &mapped, textures));
            av_frame_unref(f.frame);
        }
        QCOMPARE(finishes, 1); // teardown only
        QCOMPARE(frees.load(), 6);
    }
    void retirementKeepsDeviceAlive()
    {
        Fixture f;
        auto token = std::make_shared<int>(1);
        std::weak_ptr<int> weak = token;
        {
            PlVkFramePool pool(&f.gpu, token);
            token.reset();
            QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
        }
        QVERIFY(!weak.expired());
        av_frame_unref(f.frame);
        QVERIFY(weak.expired());
        QCOMPARE(frees.load(), 3);
    }
    void rejectsForeignAndMalformedPlanes()
    {
        Fixture f;
        PlVkFramePool pool(&f.gpu);
        pl_frame mapped{}; pl_tex textures[4]{};
        QVERIFY(av_frame_get_buffer(f.frame, 64) >= 0);
        QVERIFY(!pool.upload(f.frame, &mapped, textures));
        av_frame_unref(f.frame); f.prepare();
        QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
        uint8_t* data = f.frame->data[2];
        f.frame->data[2] = f.frame->buf[2]->data + f.frame->buf[2]->size - 1;
        QVERIFY(!pool.upload(f.frame, &mapped, textures));
        QCOMPARE(uploads, 0); // validates all planes before a partial submission
        f.frame->data[2] = data;
        f.frame->linesize[2] = INT_MIN;
        QVERIFY(!pool.upload(f.frame, &mapped, textures));
        QCOMPARE(uploads, 0);
    }
    void negativeStride()
    {
        Fixture f;
        PlVkFramePool pool(&f.gpu);
        QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
        for (int i = 0; i < 3; ++i) {
            f.frame->data[i] += f.frame->linesize[i] * (f.frame->height - 1);
            f.frame->linesize[i] = -f.frame->linesize[i];
        }
        pl_frame mapped{}; pl_tex textures[4]{};
        QVERIFY(pool.upload(f.frame, &mapped, textures));
        for (int i = 0; i < 3; ++i) {
            QVERIFY(mapped.planes[i].flipped);
            QCOMPARE(transfers[i].data.buf_offset, size_t(0));
        }
    }
    void allocationAndUploadFailures()
    {
        Fixture f;
        {
            PlVkFramePool pool(&f.gpu);
            failAllocation = 2;
            QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), AVERROR(ENOMEM));
            for (auto buffer : f.frame->buf) QVERIFY(!buffer);
            failAllocation = 0; f.prepare();
            QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
            failUpload = 2;
            pl_frame mapped{}; pl_tex textures[4]{};
            QVERIFY(!pool.upload(f.frame, &mapped, textures));
            QCOMPARE(transfers.size(), size_t(1));
            av_frame_unref(f.frame);
        }
        QCOMPARE(frees.load(), allocations.load() - 1);
    }
    void validatesAllocationBounds()
    {
        Fixture f;
        PlVkFramePool pool(&f.gpu);
        f.frame->width = INT_MAX;
        QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), AVERROR(EINVAL));
        f.prepare();
        f.gpu.limits.max_buf_size = 1;
        QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), AVERROR(ENOMEM));
        QCOMPARE(allocations.load(), 0);
        f.prepare(); f.gpu.limits.max_buf_size = INT_MAX;
        QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), 0);
        QCOMPARE(pool.getBuffer(f.codec, f.frame, 0), AVERROR(EINVAL));
    }
    void decoderThreads()
    {
        Fixture f;
        PlVkFramePool pool(&f.gpu);
        std::atomic_int errors{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
            Fixture worker;
            for (int j = 0; j < 100; ++j) {
                if (pool.getBuffer(worker.codec, worker.frame, 0) < 0) ++errors;
                av_frame_unref(worker.frame); worker.prepare();
            }
        });
        for (auto& thread : threads) thread.join();
        QCOMPARE(errors.load(), 0);
    }
    void realDecodeAndUpload_data()
    {
        QTest::addColumn<int>("codec");
        QTest::addColumn<QByteArray>("sample");
        using namespace PlankDecoderTestFrames;
#define SAMPLE(name, codecId, data) QTest::newRow(name) << int(codecId) << QByteArray(reinterpret_cast<const char*>(data), sizeof(data))
        SAMPLE("h264-8-444", AV_CODEC_ID_H264, k_h264High_444TestFrame);
        SAMPLE("h264-10-444", AV_CODEC_ID_H264, k_h264High10_444TestFrame);
        SAMPLE("h264-8-422", AV_CODEC_ID_H264, k_H264High8_422TestFrame);
        SAMPLE("h264-10-422", AV_CODEC_ID_H264, k_H264High10_422TestFrame);
        SAMPLE("hevc-8-444-identity", AV_CODEC_ID_HEVC, k_HEVCRExt8_444IdentityGbrTestFrame);
        SAMPLE("hevc-10-444-identity", AV_CODEC_ID_HEVC, k_HEVCRExt10_444IdentityGbrTestFrame);
#undef SAMPLE
    }
    void realDecodeAndUpload()
    {
        QFETCH(int, codec); QFETCH(QByteArray, sample);
        realGpu = true;
        auto device = std::make_shared<VulkanDevice>();
        auto params = pl_vulkan_default_params;
        params.allow_software = true; // isolated builder tests, never product policy
        device->vk = pl_vulkan_create(nullptr, &params);
        QVERIFY2(device->vk, "A Vulkan test driver (lavapipe on the builder) is required");
        auto gpu = device->vk->gpu;
        QVERIFY(PlVkFramePool::supported(gpu));
        PlVkFramePool pool(gpu, device);
        AVFrame* reference = decode(sample, static_cast<AVCodecID>(codec), nullptr);
        AVFrame* imported = decode(sample, static_cast<AVCodecID>(codec), &pool);
        QVERIFY(reference); QVERIFY(imported);
        QCOMPARE(imported->format, reference->format);
        QCOMPARE(imported->color_range, reference->color_range);
        QCOMPARE(imported->colorspace, reference->colorspace);
        QCOMPARE(visibleBytes(imported), visibleBytes(reference));
        QVERIFY(allocations.load() > 0);
        pl_frame mapped{}; pl_tex textures[4]{};
        QVERIFY(pool.upload(imported, &mapped, textures));
        QCOMPARE(finishes, 0);
        // Actual Vulkan transfers, not the mock above. Read back the plane data
        // in tests only and compare every visible sample, including low 10 bits.
        for (int i = 0; i < mapped.num_planes; ++i) {
            const auto texture = textures[i];
            auto readParams = texture->params;
            readParams.host_readable = true; readParams.blit_dst = true;
            pl_tex readback = pl_tex_create(gpu, &readParams);
            QVERIFY(readback);
            pl_tex_blit_params blit{};
            blit.src = texture; blit.dst = readback;
            pl_tex_blit(gpu, &blit);
            const size_t stride = texture->params.w * texture->params.format->texel_size;
            QByteArray bytes(stride * texture->params.h, '\0');
            pl_tex_transfer_params transfer{};
            transfer.tex = readback; transfer.ptr = bytes.data(); transfer.row_pitch = stride;
            QVERIFY(pl_tex_download(gpu, &transfer));
            for (int row = 0; row < texture->params.h; ++row)
                QVERIFY(memcmp(bytes.constData() + row * stride,
                               imported->data[i] + row * imported->linesize[i], stride) == 0);
            pl_tex_destroy(gpu, &readback);
            pl_tex_destroy(gpu, &textures[i]);
        }
        av_frame_free(&imported);
        av_frame_free(&reference);
    }
};

QTEST_GUILESS_MAIN(FramePoolTest)
#include "test_plvkframepool.moc"
