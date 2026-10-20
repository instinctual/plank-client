#include "videopresenter.h"
#include "decodedvideorenderer.h"

#include <QtTest>
#include <future>
#include <thread>

namespace {
QtVideoFrame::Ptr frame(QtVideoFrame::Purpose purpose = QtVideoFrame::Purpose::Live)
{
    auto* decoded = av_frame_alloc();
    if (!decoded) return {};
    decoded->width = 64; decoded->height = 32;
    decoded->format = AV_PIX_FMT_GBRP10LE;
    decoded->colorspace = AVCOL_SPC_RGB;
    decoded->color_range = AVCOL_RANGE_JPEG;
    if (av_frame_get_buffer(decoded, 64) < 0) { av_frame_free(&decoded); return {}; }
    QString error;
    auto retained = QtVideoFrame::software(decoded, AV_PIX_FMT_GBRP10LE, error, {}, purpose);
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
        auto decoded = frame(QtVideoFrame::Purpose::DecoderQualification); QVERIFY(decoded);
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
        QVERIFY(resumed.submit(frame()));
        presenter.stop();
        QVERIFY(!resumed.submit(frame()));
    }

    void qualificationIsHiddenUntilActualLivePresentation()
    {
        QQuickWindow window;
        QtVideoPresenter presenter;
        presenter.addOutput(&window, {}, true);
        auto* item = dynamic_cast<QtEglVideoItem*>(window.contentItem()->childItems().value(0));
        QVERIFY(item);
        auto delivery = presenter.deliveryFactory()();
        const auto probe = frame(QtVideoFrame::Purpose::DecoderQualification);
        const auto live = frame();
        QVERIFY(probe && probe->isQualification());
        QVERIFY(live && !live->isQualification());
        QVERIFY(!delivery.submit(probe));
        QVERIFY(!presenter.hasPresentedLiveFrame());

        const auto previous = item->presentationFeedback();
        auto qualification = std::async(std::launch::async, [&] {
            QString error;
            return delivery.qualify(probe, error);
        });
        QTRY_VERIFY(item->presentationFeedback() != previous);
        // Controlled render-feedback fixture only; real GPU draw/concealment
        // and pixel precision are independently tested in the compositor probe.
        const auto swap = [&](const QtVideoFrame::Ptr& decoded) {
            const auto feedback = item->presentationFeedback();
            {
                std::lock_guard<std::mutex> lock(feedback->presentationMutex);
                feedback->drawnFrame = decoded;
                feedback->drawStarted = std::chrono::steady_clock::now();
            }
            return QMetaObject::invokeMethod(&window, "frameSwapped", Qt::DirectConnection);
        };
        QVERIFY(swap(probe));
        QVERIFY(qualification.get());
        QVERIFY(!presenter.hasPresentedLiveFrame());
        QVERIFY(delivery.submit(live));
        QCoreApplication::processEvents();
        QVERIFY(!presenter.hasPresentedLiveFrame()); // submission is not presentation
        QVERIFY(swap(live));
        QVERIFY(presenter.hasPresentedLiveFrame());

        auto reconnected = presenter.deliveryFactory()();
        QVERIFY(!presenter.hasPresentedLiveFrame());
        QVERIFY(swap(live)); // old-generation completion cannot reveal startup
        QVERIFY(!presenter.hasPresentedLiveFrame());
        QVERIFY(!delivery.submit(live));
        QVERIFY(reconnected.submit(live));
        QCoreApplication::processEvents();
        QVERIFY(swap(live));
        QVERIFY(presenter.hasPresentedLiveFrame());
        presenter.stop();
        QVERIFY(!presenter.hasPresentedLiveFrame());
    }

    void liveFrameCannotBypassQualificationMarking()
    {
        QQuickWindow window;
        QtVideoPresenter presenter;
        presenter.addOutput(&window, {}, true);
        auto delivery = presenter.deliveryFactory()();
        auto result = std::async(std::launch::async, [&] {
            QString error;
            const bool accepted = delivery.qualify(frame(), error);
            return !accepted && error.contains("marked test frame");
        });
        QVERIFY(result.get());
        QVERIFY(!presenter.hasPresentedLiveFrame());
    }

    void failedQualificationReportsDeliveryAndRenderStages()
    {
        QQuickWindow window; // deliberately hidden: no render/swap feedback
        QtVideoPresenter presenter;
        presenter.addOutput(&window, {}, true);
        auto* item = dynamic_cast<QtEglVideoItem*>(window.contentItem()->childItems().value(0));
        QVERIFY(item);
        const auto previous = item->presentationFeedback();
        auto delivery = presenter.deliveryFactory()();
        auto result = std::async(std::launch::async, [&] {
            QString error;
            const bool accepted = delivery.qualify(frame(QtVideoFrame::Purpose::DecoderQualification), error);
            return qMakePair(accepted, error);
        });
        QTRY_VERIFY(item->presentationFeedback() != previous);
        QTRY_VERIFY(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto failure = result.get();
        QVERIFY(!failure.first);
        QVERIFY2(failure.second.contains("delivered=1"), qPrintable(failure.second));
        QVERIFY(failure.second.contains("required=1"));
        QVERIFY(failure.second.contains("delivery_visible=0"));
        QVERIFY(failure.second.contains("delivery_exposed=0"));
        QVERIFY(failure.second.contains("sync=0 imports=0 draws=0 swaps=0 error=0"));
        QVERIFY(!presenter.hasPresentedLiveFrame());
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

    void retiredFailureCannotStopANewDecoder()
    {
        QQuickWindow window;
        QtVideoPresenter presenter;
        presenter.addOutput(&window, {}, true);
        QSignalSpy failures(&presenter, &QtVideoPresenter::presentationFailed);
        auto retired = presenter.deliveryFactory()();
        retired.fail(QStringLiteral("old decoder"));
        auto current = presenter.deliveryFactory()();
        QCoreApplication::processEvents();
        QCOMPARE(failures.size(), 0);
        current.fail(QStringLiteral("current decoder"));
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(failures[0][0].toString(), QStringLiteral("current decoder"));
        QVERIFY(!current.submit(frame()));
    }
};
QTEST_MAIN(QtPresenterTest)
#include "test_qtpresenter.moc"
