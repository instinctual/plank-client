#include "vaapivideoframe.h"
#include "videomailbox.h"

#include <QtTest>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
#include <cerrno>
#include <atomic>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <thread>

namespace {
VAStatus syncStatus = VA_STATUS_SUCCESS;
VAStatus exportStatus = VA_STATUS_SUCCESS;
uint32_t sourceFormat = PlankPackedVideo::fourcc('Y','4','1','0');
int exportedFd = -1;
int syncCalls = 0;
uint64_t waitTimeout = 0;

VADRMPRIMESurfaceDescriptor descriptor()
{
    VADRMPRIMESurfaceDescriptor d{};
    d.width = 1024; d.height = 4; d.num_objects = 1; d.num_layers = 1;
    d.objects[0].fd = 42; d.objects[0].size = 16384;
    d.layers[0].drm_format = sourceFormat;
    d.layers[0].num_planes = 1;
    d.layers[0].pitch[0] = 4096;
    if (sourceFormat == DRM_FORMAT_P010) {
        d.layers[0].num_planes = 2;
        d.layers[0].pitch[0] = d.layers[0].pitch[1] = 2048;
        d.layers[0].offset[1] = 8192;
        d.objects[0].size = 12288;
    }
    return d;
}

struct FrameFixture {
    AVVAAPIDeviceContext va{};
    AVHWDeviceContext device{};
    AVFrame* frame = av_frame_alloc();
    FrameFixture()
    {
        device.type = AV_HWDEVICE_TYPE_VAAPI; device.hwctx = &va;
        frame->format = AV_PIX_FMT_VAAPI;
        frame->width = 1024; frame->height = 4;
        frame->colorspace = AVCOL_SPC_RGB; frame->color_range = AVCOL_RANGE_JPEG;
        frame->buf[0] = av_buffer_allocz(64);
        frame->data[0] = frame->buf[0]->data;
        frame->data[3] = reinterpret_cast<uint8_t*>(uintptr_t(7));
        frame->hw_frames_ctx = av_buffer_allocz(sizeof(AVHWFramesContext));
        auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
        frames->device_ctx = &device;
    }
    ~FrameFixture() { av_frame_free(&frame); }
};
}

extern "C" VAStatus __wrap_vaSyncSurface2(VADisplay, VASurfaceID, uint64_t timeout)
{
    ++syncCalls; waitTimeout = timeout; return syncStatus;
}
extern "C" VAStatus __wrap_vaExportSurfaceHandle(VADisplay, VASurfaceID, uint32_t type,
                                                  uint32_t flags, void* output)
{
    if (type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 ||
            flags != (VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (exportStatus != VA_STATUS_SUCCESS) return exportStatus;
    auto d = descriptor();
    exportedFd = open("/dev/null", O_RDONLY);
    d.objects[0].fd = exportedFd;
    *static_cast<VADRMPRIMESurfaceDescriptor*>(output) = d;
    return VA_STATUS_SUCCESS;
}

class QtVideoTest : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        syncStatus = exportStatus = VA_STATUS_SUCCESS;
        sourceFormat = PlankPackedVideo::fourcc('Y','4','1','0');
        exportedFd = -1; syncCalls = 0; waitTimeout = 0;
    }

    void storageMapping()
    {
        using namespace PlankPackedVideo;
        QCOMPARE(importFormat(fourcc('Y','4','1','0'), true, false), fourcc('X','R','3','0'));
        QCOMPARE(importFormat(fourcc('Y','4','1','0'), false, true), fourcc('X','R','3','0'));
        QCOMPARE(importFormat(fourcc('A','Y','U','V'), true, false), fourcc('X','R','2','4'));
        QCOMPARE(importFormat(fourcc('X','Y','U','V'), true, false), fourcc('X','R','2','4'));
        QCOMPARE(importFormat(fourcc('N','V','1','2'), true, false), fourcc('N','V','1','2'));
        QCOMPARE(importFormat(fourcc('Y','4','1','0'), false, false), fourcc('Y','4','1','0'));
        QCOMPARE(mapping(fourcc('Y','4','1','0'), 8, true, false, true), Mapping::Unsupported);
        QCOMPARE(mapping(fourcc('Y','4','1','0'), 10, true, false, false), Mapping::Unsupported);
    }

    void rejectsMalformedLayout()
    {
        auto valid = [](const auto& d) { return QtVaapiVideoFrame::validate(d, {1024, 4}, 10, true, false, true); };
        auto d = descriptor(); QVERIFY(valid(d));
        d.num_objects = 5; QVERIFY(!valid(d)); d = descriptor();
        d.num_layers = 0; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].num_planes = 2; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].object_index[0] = 1; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].pitch[0] = 4095; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].pitch[0] = UINT32_MAX; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].offset[0] = d.objects[0].size; QVERIFY(!valid(d)); d = descriptor();
        d.objects[0].fd = -1; QVERIFY(!valid(d)); d = descriptor();
        d.objects[0].size -= 1; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].offset[0] = 4; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].offset[0] = 4; d.objects[0].size += 4; QVERIFY(valid(d)); d = descriptor();
        d.layers[0].pitch[0] = 8192; QVERIFY(!valid(d)); d = descriptor();
        d.objects[0].drm_format_modifier = DRM_FORMAT_MOD_INVALID; QVERIFY(valid(d)); d = descriptor();
        d.width = 1023; QVERIFY(!valid(d)); d = descriptor();
        QVERIFY(!QtVaapiVideoFrame::validate(d, {0, 4}, 10, true, false, true));
        QVERIFY(!QtVaapiVideoFrame::validate(d, {1024, 4}, 10, true, false, false));
        QVERIFY(!QtVaapiVideoFrame::validate(d, {1024, 4}, 8, true, false, true));
    }

    void ownsDecoderReferenceAndDescriptors()
    {
        FrameFixture fixture;
        QString error;
        auto lease = QtVaapiVideoFrame::create(fixture.frame, 10, error);
        QVERIFY2(lease, qPrintable(error));
        QCOMPARE(av_buffer_get_ref_count(fixture.frame->buf[0]), 2);
        QCOMPARE(lease->mapping(), PlankPackedVideo::Mapping::Identity10);
        QCOMPARE(lease->size(), QSize(1024, 4));
        QVERIFY(fcntl(exportedFd, F_GETFD) & FD_CLOEXEC);
        QCOMPARE(syncCalls, 1);
        QCOMPARE(waitTimeout, uint64_t(100000000));
        auto retained = lease;
        lease.reset();
        QCOMPARE(av_buffer_get_ref_count(fixture.frame->buf[0]), 2);
        retained.reset();
        QCOMPARE(av_buffer_get_ref_count(fixture.frame->buf[0]), 1);
        QCOMPARE(fcntl(exportedFd, F_GETFD), -1);
        QCOMPARE(errno, EBADF);
    }

    void p010RequiresExactTenBit420AndPlaneBounds()
    {
        sourceFormat = DRM_FORMAT_P010;
        auto valid = [](const auto& d) { return QtVaapiVideoFrame::validate(d, {1024, 4}, 10, false, true, true, true); };
        auto d = descriptor(); QVERIFY(valid(d));
        QVERIFY(!QtVaapiVideoFrame::validate(d, {1024, 4}, 10, false, true, true));
        QVERIFY(!QtVaapiVideoFrame::validate(d, {1024, 4}, 8, false, true, true, true));
        QVERIFY(!QtVaapiVideoFrame::validate(d, {1024, 4}, 10, true, false, true, true));
        d.layers[0].num_planes = 3; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].pitch[1] = 2047; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].object_index[1] = 2; QVERIFY(!valid(d)); d = descriptor();
        d.layers[0].offset[1]++; QVERIFY(!valid(d)); d = descriptor();
        d.num_objects = 2; QVERIFY(!valid(d));
        d.objects[1] = d.objects[0]; d.objects[1].size = 4096;
        d.layers[0].object_index[1] = 1; d.layers[0].offset[1] = 0;
        QVERIFY(valid(d));
        d.objects[1].size--; QVERIFY(!valid(d));

        FrameFixture fixture;
        fixture.frame->colorspace = AVCOL_SPC_BT709;
        QString error;
        QVERIFY(!QtVaapiVideoFrame::create(fixture.frame, 10, error, true));
        reinterpret_cast<AVHWFramesContext*>(fixture.frame->hw_frames_ctx->data)->sw_format = AV_PIX_FMT_P010LE;
        auto lease = QtVaapiVideoFrame::create(fixture.frame, 10, error, true);
        QVERIFY2(lease, qPrintable(error));
        QVERIFY(lease->planar());
        QCOMPARE(lease->mapping(), PlankPackedVideo::Mapping::Bt709Full10);
        QCOMPARE(lease->importFormat(), uint32_t(DRM_FORMAT_R16));
        QCOMPARE(lease->importFormat(1), uint32_t(DRM_FORMAT_GR1616));
        QCOMPARE(lease->planeSize(1), QSize(512, 2));
        QVERIFY(fcntl(exportedFd, F_GETFD) & FD_CLOEXEC);
        lease.reset();
        QCOMPARE(fcntl(exportedFd, F_GETFD), -1);
        QCOMPARE(av_buffer_get_ref_count(fixture.frame->buf[0]), 1);
    }

    void rejectBeforeTouchingDevice()
    {
        FrameFixture fixture;
        QString error;
        QVERIFY(!QtVaapiVideoFrame::create(nullptr, 10, error));
        fixture.frame->color_range = AVCOL_RANGE_MPEG;
        QVERIFY(!QtVaapiVideoFrame::create(fixture.frame, 10, error));
        fixture.frame->color_range = AVCOL_RANGE_JPEG;
        fixture.frame->colorspace = AVCOL_SPC_UNSPECIFIED;
        QVERIFY(!QtVaapiVideoFrame::create(fixture.frame, 10, error));
        QCOMPARE(syncCalls, 0);
    }

    void rejectsAndReleasesFailedExport()
    {
        FrameFixture fixture;
        QString error;
        sourceFormat = PlankPackedVideo::fourcc('N','V','1','2');
        QVERIFY(!QtVaapiVideoFrame::create(fixture.frame, 10, error));
        QCOMPARE(fcntl(exportedFd, F_GETFD), -1);
        QCOMPARE(errno, EBADF);
        QCOMPARE(av_buffer_get_ref_count(fixture.frame->buf[0]), 1);
        syncStatus = VA_STATUS_ERROR_TIMEDOUT;
        QVERIFY(!QtVaapiVideoFrame::create(fixture.frame, 10, error));
        QCOMPARE(av_buffer_get_ref_count(fixture.frame->buf[0]), 1);
    }

    void formatChangesAreExplicit()
    {
        FrameFixture fixture;
        QString error;
        fixture.frame->colorspace = AVCOL_SPC_BT709;
        auto bt709 = QtVaapiVideoFrame::create(fixture.frame, 10, error);
        QVERIFY2(bt709, qPrintable(error));
        QCOMPARE(bt709->mapping(), PlankPackedVideo::Mapping::Bt709Full10);
        fixture.frame->color_range = AVCOL_RANGE_MPEG;
        auto limited = QtVaapiVideoFrame::create(fixture.frame, 10, error);
        QVERIFY2(limited, qPrintable(error));
        QCOMPARE(limited->mapping(), PlankPackedVideo::Mapping::Bt709Limited10);
        QCOMPARE(limited->importFormat(), PlankPackedVideo::fourcc('X','R','3','0'));
        fixture.frame->color_range = AVCOL_RANGE_UNSPECIFIED;
        QVERIFY(!QtVaapiVideoFrame::create(fixture.frame, 10, error));
        fixture.frame->color_range = AVCOL_RANGE_JPEG;
        fixture.frame->colorspace = AVCOL_SPC_RGB;
        sourceFormat = PlankPackedVideo::fourcc('X','Y','U','V');
        QVERIFY(!QtVaapiVideoFrame::create(fixture.frame, 10, error));
        auto identity8 = QtVaapiVideoFrame::create(fixture.frame, 8, error);
        QVERIFY2(identity8, qPrintable(error));
        QCOMPARE(identity8->mapping(), PlankPackedVideo::Mapping::Identity8);
        QCOMPARE(bt709->mapping(), PlankPackedVideo::Mapping::Bt709Full10);
    }

    void coalescesFramesWithoutBlockingProducer()
    {
        FrameFixture fixture;
        QString error;
        auto first = QtVideoFrame::hardware(QtVaapiVideoFrame::create(fixture.frame, 10, error));
        auto last = QtVideoFrame::hardware(QtVaapiVideoFrame::create(fixture.frame, 10, error));
        QVERIFY(first && last);
        int deliveries = 0;
        QtVideoFrame::Ptr received;
        QtVideoMailbox queue([&](auto frame) {
            QCOMPARE(QThread::currentThread(), thread());
            ++deliveries; received = std::move(frame);
        });
        const auto producer = queue.startGeneration();
        std::atomic_bool submitted{true};
        std::thread worker([&] {
            for (int i = 0; i < 10000; ++i) if (!producer.submit(first)) submitted = false;
            if (!producer.submit(last)) submitted = false;
        });
        worker.join(); // deliberately do not dispatch GUI work until submission finishes
        QVERIFY(submitted.load());
        QCOMPARE(deliveries, 0);
        QCOMPARE(queue.stats().submitted, uint64_t(10001));
        QCOMPARE(queue.stats().replaced, uint64_t(10000));
        QVERIFY(queue.stats().pending);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(deliveries, 1);
        QCOMPARE(received, last);
        QVERIFY(!queue.stats().pending);
    }

    void reconnectRejectsOldProducerAndPendingFrames()
    {
        FrameFixture fixture;
        QString error;
        auto first = QtVideoFrame::hardware(QtVaapiVideoFrame::create(fixture.frame, 10, error));
        auto last = QtVideoFrame::hardware(QtVaapiVideoFrame::create(fixture.frame, 10, error));
        QVERIFY(first && last);
        int deliveries = 0;
        QtVideoFrame::Ptr received;
        QtVideoMailbox queue([&](auto frame) { ++deliveries; received = std::move(frame); });
        const auto old = queue.startGeneration();
        QVERIFY(old.submit(first));
        const auto current = queue.startGeneration();
        QVERIFY(!old.submit(first));
        QVERIFY(current.submit(last));
        QCoreApplication::sendPostedEvents();
        QCOMPARE(deliveries, 1);
        QCOMPARE(received, last);
        QVERIFY(current.submit(first));
        queue.retire();
        QVERIFY(!current.submit(last));
        QCoreApplication::sendPostedEvents();
        QCOMPARE(deliveries, 1);
    }

    void teardownRejectsLiveWorkerWithoutGuiWait()
    {
        FrameFixture fixture;
        QString error;
        auto frame = QtVideoFrame::hardware(QtVaapiVideoFrame::create(fixture.frame, 10, error));
        QVERIFY(frame);
        int deliveries = 0;
        for (int iteration = 0; iteration < 100; ++iteration) {
            auto queue = std::make_unique<QtVideoMailbox>([&](auto) { ++deliveries; });
            const auto producer = queue->startGeneration();
            std::atomic_bool submitted{false};
            std::thread worker([&] {
                for (int attempt = 0; attempt < 10000; ++attempt) {
                    if (!producer.submit(frame)) break;
                    submitted.store(true);
                }
            });
            while (!submitted.load()) std::this_thread::yield();
            queue.reset();
            worker.join();
            QVERIFY(!producer.submit(frame));
        }
        QCoreApplication::sendPostedEvents();
        QCOMPARE(deliveries, 0);
    }

    void consumerCanCloseItsOwnSession()
    {
        FrameFixture fixture;
        QString error;
        auto frame = QtVideoFrame::hardware(QtVaapiVideoFrame::create(fixture.frame, 10, error));
        QVERIFY(frame);
        std::unique_ptr<QtVideoMailbox> queue;
        int completed = 0;
        queue = std::make_unique<QtVideoMailbox>([&](auto delivered) {
            queue.reset();
            // The callable and its captures must still be alive after close.
            QCOMPARE(delivered, frame);
            ++completed;
        });
        const auto producer = queue->startGeneration();
        QVERIFY(producer.submit(frame));
        QCoreApplication::sendPostedEvents();
        QCOMPARE(completed, 1);
        QVERIFY(!queue);
        QVERIFY(!producer.submit(frame));
    }

    void softwareDeliveryRetainsExactFrameWithoutPixelCopy()
    {
        AVFrame* source = av_frame_alloc();
        QVERIFY(source);
        source->format = AV_PIX_FMT_GBRP10LE;
        source->width = 1024; source->height = 8;
        QVERIFY(av_frame_get_buffer(source, 64) == 0);
        QString error;
        QVERIFY(!QtVideoFrame::software(source, AV_PIX_FMT_GBRP, error));
        auto frame = QtVideoFrame::software(source, AV_PIX_FMT_GBRP10LE, error);
        QVERIFY2(frame, qPrintable(error));
        QCOMPARE(frame->size(), QSize(1024, 8));
        QCOMPARE(frame->softwareFormat(), AV_PIX_FMT_GBRP10LE);
        QCOMPARE(frame->softwareFrame()->data[0], source->data[0]);
        QCOMPARE(av_buffer_get_ref_count(source->buf[0]), 2);
        QtVideoFrame::Ptr received;
        QtVideoMailbox mailbox([&](auto f) { received = std::move(f); });
        auto producer = mailbox.startGeneration();
        QVERIFY(producer.submit(frame));
        av_frame_free(&source);
        QCoreApplication::sendPostedEvents();
        QCOMPARE(received, frame);
        QVERIFY(!received->hardwareFrame());
        QCOMPARE(av_buffer_get_ref_count(received->softwareFrame()->buf[0]), 1);
    }

    void presentationFeedbackIsOnceOnlyAndNeverAtSubmission()
    {
        FrameFixture fixture;
        QString error;
        auto lease = QtVaapiVideoFrame::create(fixture.frame, 10, error);
        std::atomic<int> displayed{0}, dropped{0};
        auto frame = QtVideoFrame::hardware(lease, [&](bool presented, uint64_t, uint64_t renderMs) {
            if (presented) { QVERIFY(renderMs == 7); ++displayed; }
            else ++dropped;
        });
        QtVideoFrame::Ptr retained;
        QtVideoMailbox mailbox([&](auto delivered) { retained = std::move(delivered); });
        QVERIFY(mailbox.startGeneration().submit(frame));
        QCoreApplication::sendPostedEvents();
        QCOMPARE(displayed.load(), 0);
        std::thread first([frame] { frame->presented(7); });
        std::thread second([frame] { frame->presented(7); });
        first.join(); second.join();
        QCOMPARE(displayed.load(), 1);
        retained.reset(); frame.reset();
        QCOMPARE(dropped.load(), 0);
        auto discarded = QtVideoFrame::hardware(lease, [&](bool presented, uint64_t, uint64_t) {
            QVERIFY(!presented); ++dropped;
        });
        discarded.reset();
        QCOMPARE(dropped.load(), 1);
    }

    void decoderGenerationRetirementCannotDiscardReplacement()
    {
        FrameFixture fixture;
        QString error;
        auto frame = QtVideoFrame::hardware(QtVaapiVideoFrame::create(fixture.frame, 10, error));
        int received = 0;
        auto mailbox = std::make_unique<QtVideoMailbox>([&](auto) { ++received; });
        auto old = mailbox->startGeneration();
        QVERIFY(old.submit(frame));
        QtVideoMailbox::Producer current;
        std::thread restart([&] { current = old.restart(); old.discard(); });
        restart.join();
        QVERIFY(!old.isCurrent()); QVERIFY(current.isCurrent());
        QVERIFY(!old.submit(frame)); QVERIFY(current.submit(frame));
        QCoreApplication::sendPostedEvents();
        QCOMPARE(received, 1);
        current.discard();
        QVERIFY(!current.isCurrent()); QVERIFY(!current.submit(frame));
        auto resumed = current.restart();
        QVERIFY(resumed.isCurrent());
        mailbox.reset();
        QVERIFY(!resumed.isCurrent()); QVERIFY(!resumed.restart().isCurrent());
    }

    void claimedFrameKeepsItsOriginalGeneration()
    {
        FrameFixture fixture;
        QString error;
        auto frame = QtVideoFrame::hardware(QtVaapiVideoFrame::create(fixture.frame, 10, error));
        QtVideoMailbox::Producer next;
        bool checked = false;
        QtVideoMailbox mailbox([&](QtVideoFrame::Ptr received, QtVideoMailbox::Producer claimed) {
            QCOMPARE(received, frame);
            QVERIFY(claimed.isCurrent());
            next = claimed.restart();
            QVERIFY(!claimed.isCurrent());
            QVERIFY(next.isCurrent());
            checked = true;
        });
        QVERIFY(mailbox.startGeneration().submit(frame));
        QCoreApplication::sendPostedEvents();
        QVERIFY(checked);
    }
};
QTEST_GUILESS_MAIN(QtVideoTest)
#include "test_qtvideo.moc"
