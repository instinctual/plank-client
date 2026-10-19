#include "videopresenter.h"
#include "decodedvideorenderer.h"

#include <QtTest>
#include <future>
#include <thread>

namespace {
QtVideoFrame::Ptr frame()
{
    auto* decoded = av_frame_alloc();
    if (!decoded) return {};
    decoded->width = 64; decoded->height = 32;
    decoded->format = AV_PIX_FMT_GBRP10LE;
    decoded->colorspace = AVCOL_SPC_RGB;
    decoded->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(decoded, 64) < 0) { av_frame_free(&decoded); return {}; }
    QString error;
    auto retained = QtVideoFrame::software(decoded, AV_PIX_FMT_GBRP10LE, error);
    av_frame_free(&decoded);
    return retained;
}
}

class QtPresenterTest : public QObject
{
    Q_OBJECT
private slots:
    void noGpuIsNotPresentationAcceptance()
    {
        QtDecodedVideoRendererFactory factory({}, {}, [] { return QtDecodedVideoDelivery{}; });
        std::unique_ptr<IFFmpegRenderer> software(factory.createRenderer(nullptr));
        QVERIFY(software);
        QVERIFY(software->needsTestFrame());
        QVERIFY(software->ownsPresentationScheduling());
        DECODER_PARAMETERS params{};
        params.videoFormat = VIDEO_FORMAT_H264_HIGH10_444;
        params.enableIdentityGbr = true;
        QVERIFY(!software->initialize(&params));
        AVCodecHWConfig unsupported{};
        unsupported.device_type = AV_HWDEVICE_TYPE_CUDA;
        unsupported.pix_fmt = AV_PIX_FMT_CUDA;
        QVERIFY(!factory.createRenderer(&unsupported));
    }

    void qualificationCannotBlockTheGui()
    {
        QQuickWindow window;
        QtVideoPresenter presenter;
        presenter.addOutput(&window, {}, true);
        auto delivery = presenter.deliveryFactory()();
        QVERIFY(delivery.qualify);
        QString error;
        QVERIFY(!delivery.qualify(frame(), error));
        QVERIFY(error.contains("GUI thread"));
    }

    void cancelledQualificationDoesNotWaitForTheTimeout()
    {
        QQuickWindow window;
        QtVideoPresenter presenter;
        presenter.addOutput(&window, {}, true);
        auto delivery = presenter.deliveryFactory()();
        auto decoded = frame(); QVERIFY(decoded);
        std::promise<void> started;
        std::promise<bool> result;
        auto finished = result.get_future();
        std::thread worker([&] {
            started.set_value();
            QString error;
            result.set_value(delivery.qualify(decoded, error));
        });
        started.get_future().wait();
        QTest::qWait(20);
        delivery.discard();
        const auto ready = finished.wait_for(std::chrono::milliseconds(500));
        worker.join();
        QCOMPARE(ready, std::future_status::ready);
        QVERIFY(!finished.get());
        QVERIFY(!delivery.submit(decoded));
        auto resumed = presenter.deliveryFactory()();
        QVERIFY(resumed.submit(decoded));
        presenter.stop();
        QVERIFY(!resumed.submit(decoded));
    }

    void destroyedPresenterRetiresWorkerHandles()
    {
        QQuickWindow window;
        auto presenter = std::make_unique<QtVideoPresenter>();
        presenter->addOutput(&window, {}, true);
        auto factory = presenter->deliveryFactory();
        auto delivery = factory();
        presenter.reset();
        QVERIFY(!delivery.submit(frame()));
        QString error;
        QVERIFY(!factory().submit);
        delivery.discard();
    }
};
QTEST_MAIN(QtPresenterTest)
#include "test_qtpresenter.moc"
