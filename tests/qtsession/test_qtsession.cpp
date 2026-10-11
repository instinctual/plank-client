#include <QtTest>
#include <QGuiApplication>
#include <QQuickItem>
#include <QSignalSpy>
#include <limits>

#include "sessionwindow.h"

namespace {
QtSessionGeometry fullFrame(const QSize& logical = {1280, 720}, qreal scale = 1.0)
{
    const QSize backing(qRound(logical.width() * scale), qRound(logical.height() * scale));
    return {{3840, 2160}, {0, 0, 3840, 2160}, {QPointF(), backing}, backing, logical};
}

void mouse(QtSessionWindow& window, QEvent::Type type, QPointF position,
           Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = {})
{
    QMouseEvent event(type, position, position, button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &event);
}

void prepare(QtSessionWindow& window)
{
    window.resize(1280, 720);
    window.show();
    QCoreApplication::processEvents();
    window.setPresentation(fullFrame());
    window.setRemoteInputEnabled(true);
}

QPointF controlCenter(QtSessionWindow& window, const char* name)
{
    const auto item = window.rootObject()->findChild<QQuickItem*>(QString::fromLatin1(name));
    Q_ASSERT(item);
    return item->mapToScene({item->width() / 2, item->height() / 2});
}
}

class QtSessionTest : public QObject
{
    Q_OBJECT
private slots:
    void coordinateScale_data()
    {
        QTest::addColumn<qreal>("scale");
        QTest::newRow("100-percent") << 1.0;
        QTest::newRow("125-percent") << 1.25;
        QTest::newRow("150-percent") << 1.5;
        QTest::newRow("200-percent") << 2.0;
    }

    void coordinateScale()
    {
        QFETCH(qreal, scale);
        const auto geometry = fullFrame({1280, 720}, scale);
        QPointF remote, local;
        for (const QPointF point : {QPointF(0, 0), QPointF(177.25, 241.75), QPointF(640, 360)}) {
            QVERIFY(geometry.toStream(point, remote));
            QCOMPARE(remote, point * 3);
            QVERIFY(geometry.fromStream(remote, local));
            QCOMPARE(local, point);
        }
    }

    void letterboxAndDrag()
    {
        QtSessionGeometry geometry{{5120, 2160}, {0, 0, 5120, 2160},
                                   {0, 360, 2560, 1080}, {2560, 1800}, {2048, 1440}};
        QPointF position;
        QVERIFY(geometry.valid());
        QVERIFY(!geometry.toStream({1024, 0}, position));
        QVERIFY(!geometry.toStream({1024, 1200}, position));
        QVERIFY(geometry.toStream({1024, 720}, position));
        QCOMPARE(position, QPointF(2560, 1080));
        QVERIFY(geometry.toStream({-100, -100}, position, true));
        QCOMPARE(position, QPointF(0, 0));
        QVERIFY(geometry.toStream({10000, 10000}, position, true));
        QCOMPARE(position, QPointF(5119, 2159));
    }

    void unequalOutputSlices()
    {
        const QSize canvas(5120, 2160);
        QtSessionGeometry left{canvas, {0, 0, 3840, 2160}, {0, 0, 3840, 2160},
                               {3840, 2160}, {3072, 1728}};
        QtSessionGeometry right{canvas, {3840, 0, 1280, 2160}, {0, 0, 1280, 2160},
                                {1280, 2160}, {1280, 2160}};
        QPointF position;
        QVERIFY(!left.fromStream({3840, 1080}, position));
        QVERIFY(right.fromStream({3840, 1080}, position));
        QCOMPARE(position, QPointF(0, 1080));
        QVERIFY(right.toStream({640, 1080}, position));
        QCOMPARE(position, QPointF(4480, 1080));
        QVERIFY(left.toStream({1536, 864}, position));
        QCOMPARE(position, QPointF(1920, 1080));
    }

    void invalidGeometry()
    {
        QPointF position;
        QtSessionGeometry empty;
        QVERIFY(!empty.toStream({}, position, true));
        for (int component = 0; component < 6; ++component) {
            auto geometry = fullFrame();
            if (component == 0) geometry.logicalSize.setWidth(0);
            if (component == 1) geometry.drawableSize.setHeight(0);
            if (component == 2) geometry.sourcePixels.setX(-1);
            if (component == 3) geometry.destinationPixels.setWidth(2000);
            if (component == 4) geometry.sourcePixels.setY(std::numeric_limits<qreal>::quiet_NaN());
            if (component == 5) geometry.destinationPixels.setWidth(std::numeric_limits<qreal>::infinity());
            QVERIFY(!geometry.valid());
            QVERIFY(!geometry.toStream({}, position, true));
        }
        QVERIFY(!fullFrame().toStream({std::numeric_limits<qreal>::infinity(), 1}, position, true));
        QVERIFY(!fullFrame().toStream({1280, 360}, position));
    }

    void qmlLoads()
    {
        QtSessionWindow window;
        QCOMPARE(window.status(), QQuickView::Ready);
        QVERIFY(window.rootObject());
        QVERIFY(window.rootObject()->findChild<QQuickItem*>("sessionToolbar"));
    }

    void remoteDragCrossesToolbar()
    {
        QtSessionWindow window;
        prepare(window);
        window.setPinned(true);
        QSignalSpy buttons(&window, &QtSessionWindow::remoteButton);
        QSignalSpy motion(&window, &QtSessionWindow::remoteMotion);
        QSignalSpy disconnect(&window, &QtSessionWindow::disconnectRequested);
        const auto closeButton = controlCenter(window, "disconnectButton");
        mouse(window, QEvent::MouseButtonPress, {640, 360}, Qt::LeftButton, Qt::LeftButton);
        mouse(window, QEvent::MouseMove, closeButton, Qt::NoButton, Qt::LeftButton);
        mouse(window, QEvent::MouseButtonRelease, closeButton, Qt::LeftButton);
        QCOMPARE(buttons.count(), 2);
        QCOMPARE(buttons.at(0).at(1).toBool(), true);
        QCOMPARE(buttons.at(1).at(1).toBool(), false);
        QCOMPARE(motion.count(), 1);
        QCOMPARE(disconnect.count(), 0);
    }

    void localDragDoesNotReachHost()
    {
        QtSessionWindow window;
        prepare(window);
        window.setPinned(true);
        QSignalSpy buttons(&window, &QtSessionWindow::remoteButton);
        QSignalSpy motion(&window, &QtSessionWindow::remoteMotion);
        const auto handle = controlCenter(window, "toolbarHandle");
        mouse(window, QEvent::MouseButtonPress, handle, Qt::LeftButton, Qt::LeftButton);
        mouse(window, QEvent::MouseMove, {handle.x() - 100, handle.y()}, Qt::NoButton, Qt::LeftButton);
        QVERIFY(window.toolbarPosition() < 0.5);
        mouse(window, QEvent::MouseMove, {1100, handle.y()}, Qt::NoButton, Qt::LeftButton);
        QVERIFY(window.toolbarPosition() > 0.5);
        mouse(window, QEvent::MouseButtonRelease, {1100, 600}, Qt::LeftButton);
        QCOMPARE(buttons.count(), 0);
        QCOMPARE(motion.count(), 0);
        mouse(window, QEvent::MouseMove, {600, 600});
        QCOMPARE(motion.count(), 1);
    }

    void localButtonsRemainInteractive()
    {
        QtSessionWindow window;
        prepare(window);
        window.setPinned(true);
        QSignalSpy disconnect(&window, &QtSessionWindow::disconnectRequested);
        QSignalSpy remote(&window, &QtSessionWindow::remoteButton);
        const auto point = controlCenter(window, "disconnectButton");
        mouse(window, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        mouse(window, QEvent::MouseButtonRelease, point, Qt::LeftButton);
        QCOMPARE(disconnect.count(), 1);
        QCOMPARE(remote.count(), 0);
    }

    void focusLossReleasesOnce()
    {
        QtSessionWindow window;
        prepare(window);
        QSignalSpy buttons(&window, &QtSessionWindow::remoteButton);
        QSignalSpy keys(&window, &QtSessionWindow::remoteKey);
        mouse(window, QEvent::MouseButtonPress, {500, 300}, Qt::LeftButton, Qt::LeftButton);
        mouse(window, QEvent::MouseButtonPress, {500, 300}, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, 50, 0, 0);
        QCoreApplication::sendEvent(&window, &down);
        QFocusEvent lost(QEvent::FocusOut);
        QCoreApplication::sendEvent(&window, &lost);
        QCOMPARE(buttons.count(), 4);
        QCOMPARE(keys.count(), 2);
        QVERIFY(!keys.last().at(2).toBool());
        mouse(window, QEvent::MouseButtonRelease, {500, 300}, Qt::LeftButton);
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier, 50, 0, 0);
        QCoreApplication::sendEvent(&window, &up);
        QCoreApplication::sendEvent(&window, &lost);
        QCOMPARE(buttons.count(), 4);
        QCOMPARE(keys.count(), 2);
    }

    void resizeInvalidatesInputUntilNewViewport()
    {
        QtSessionWindow window;
        prepare(window);
        QSignalSpy buttons(&window, &QtSessionWindow::remoteButton);
        QSignalSpy motion(&window, &QtSessionWindow::remoteMotion);
        mouse(window, QEvent::MouseButtonPress, {500, 300}, Qt::LeftButton, Qt::LeftButton);
        window.resize(1024, 576);
        QCoreApplication::processEvents();
        QCOMPARE(buttons.count(), 2);
        mouse(window, QEvent::MouseMove, {512, 288});
        QCOMPARE(motion.count(), 0);
        window.setPresentation(fullFrame()); // Stale logical size must be rejected.
        mouse(window, QEvent::MouseMove, {512, 288});
        QCOMPARE(motion.count(), 0);
        window.setPresentation(fullFrame(window.size()));
        mouse(window, QEvent::MouseMove, {512, 288});
        QCOMPARE(motion.count(), 1);
        QCOMPARE(motion.last().at(0).toPointF(), QPointF(1920, 1080));
    }

    void reconnectReleasesAndKeepsCursorVisible()
    {
        QtSessionWindow window;
        prepare(window);
        window.setRemoteCursor(QCursor(Qt::BlankCursor));
        QSignalSpy buttons(&window, &QtSessionWindow::remoteButton);
        QSignalSpy motion(&window, &QtSessionWindow::remoteMotion);
        mouse(window, QEvent::MouseButtonPress, {500, 300}, Qt::LeftButton, Qt::LeftButton);
        window.setWaiting(true);
        QCOMPARE(buttons.count(), 2);
        QCOMPARE(window.cursor().shape(), Qt::ArrowCursor);
        mouse(window, QEvent::MouseMove, {600, 350});
        QCOMPARE(motion.count(), 0);
        QCOMPARE(window.cursor().shape(), Qt::ArrowCursor);
        window.setWaiting(false);
        mouse(window, QEvent::MouseMove, {600, 350});
        QCOMPARE(motion.count(), 1);
    }

    void revealUsesWindowEdgeAndDwell()
    {
        QtSessionWindow window;
        prepare(window);
        window.setPresentation({{5120, 2160}, {0, 0, 5120, 2160},
                                {0, 90, 1280, 540}, {1280, 720}, {1280, 720}});
        mouse(window, QEvent::MouseMove, {1270, 0});
        QVERIFY(!window.toolbarVisible());
        QTest::qWait(400);
        mouse(window, QEvent::MouseMove, {1270, 20});
        QTest::qWait(700);
        QVERIFY(!window.toolbarVisible());
        mouse(window, QEvent::MouseMove, {1270, 0});
        QTRY_VERIFY_WITH_TIMEOUT(window.toolbarVisible(), 1600);
        // No further motion is necessary for the five-second hide timeout.
        QTRY_VERIFY_WITH_TIMEOUT(!window.toolbarVisible(), 6000);
    }

    void bitrateClampAndNoDuplicateSignal()
    {
        QtSessionWindow window;
        QSignalSpy requested(&window, &QtSessionWindow::bitrateRequested);
        window.setBitrateKbps(99500);
        window.setBitrateKbps(99500);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(window.bitrateKbps(), 99500);
        window.setBitrateKbps(std::numeric_limits<int>::max());
        QCOMPARE(window.bitrateKbps(), 150000);
        window.setBitrateKbps(-1);
        QCOMPARE(window.bitrateKbps(), 10000);
    }

    void sliderDoesNotLeakButtonsOrWheel()
    {
        QtSessionWindow window;
        prepare(window);
        window.setPinned(true);
        QSignalSpy buttons(&window, &QtSessionWindow::remoteButton);
        QSignalSpy requested(&window, &QtSessionWindow::bitrateRequested);
        QSignalSpy wheel(&window, &QtSessionWindow::remoteWheel);
        const auto point = controlCenter(window, "bitrateSlider");
        mouse(window, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        mouse(window, QEvent::MouseMove, point + QPointF(30, 0), Qt::NoButton, Qt::LeftButton);
        mouse(window, QEvent::MouseButtonRelease, point + QPointF(30, 0), Qt::LeftButton);
        QVERIFY(requested.count() > 0);
        QCOMPARE(buttons.count(), 0);
        QWheelEvent scroll(point, point, {}, {0, 120}, {}, {}, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&window, &scroll);
        QCOMPARE(wheel.count(), 0);
        QWheelEvent remoteScroll({640, 360}, {640, 360}, {}, {0, 120}, {}, {}, Qt::NoScrollPhase, true);
        QCoreApplication::sendEvent(&window, &remoteScroll);
        QCOMPARE(wheel.count(), 1);
        QCOMPARE(wheel.last().at(0).toPoint(), QPoint(0, 120));
        QVERIFY(wheel.last().at(2).toBool());
    }

    void ignoredSynthesizedMouse()
    {
        QtSessionWindow window;
        prepare(window);
        QSignalSpy buttons(&window, &QtSessionWindow::remoteButton);
        QSignalSpy motion(&window, &QtSessionWindow::remoteMotion);
        const QPointF p(500, 300);
        QMouseEvent down(QEvent::MouseButtonPress, p, p, p, Qt::LeftButton,
                         Qt::LeftButton, {}, Qt::MouseEventSynthesizedByQt);
        QMouseEvent move(QEvent::MouseMove, p, p, p, Qt::NoButton,
                         Qt::LeftButton, {}, Qt::MouseEventSynthesizedByQt);
        QMouseEvent up(QEvent::MouseButtonRelease, p, p, p, Qt::LeftButton,
                       {}, {}, Qt::MouseEventSynthesizedByQt);
        QCoreApplication::sendEvent(&window, &down);
        QCoreApplication::sendEvent(&window, &move);
        QCoreApplication::sendEvent(&window, &up);
        QCOMPARE(buttons.count(), 0);
        QCOMPARE(motion.count(), 0);
    }

    void autoRepeatDoesNotLoseHeldKey()
    {
        QtSessionWindow window;
        prepare(window);
        QSignalSpy keys(&window, &QtSessionWindow::remoteKey);
        QKeyEvent down(QEvent::KeyPress, Qt::Key_A, {}, 38, 0, 0);
        QKeyEvent repeatUp(QEvent::KeyRelease, Qt::Key_A, {}, 38, 0, 0, {}, true);
        QKeyEvent repeatDown(QEvent::KeyPress, Qt::Key_A, {}, 38, 0, 0, {}, true);
        QCoreApplication::sendEvent(&window, &down);
        QCoreApplication::sendEvent(&window, &repeatUp);
        QCoreApplication::sendEvent(&window, &repeatDown);
        QCOMPARE(keys.count(), 2);
        QVERIFY(keys.last().at(3).toBool());
        window.setRemoteInputEnabled(false);
        QCOMPARE(keys.count(), 3);
        QVERIFY(!keys.last().at(2).toBool());
        window.setRemoteInputEnabled(true);
        QCoreApplication::sendEvent(&window, &repeatDown);
        QCOMPARE(keys.count(), 3);
    }

    void destroyReleasesHeldInput()
    {
        auto window = std::make_unique<QtSessionWindow>();
        prepare(*window);
        int releases = 0;
        connect(window.get(), &QtSessionWindow::remoteButton, this,
                [&releases](int, bool down, QPointF) { if (!down) ++releases; });
        mouse(*window, QEvent::MouseButtonPress, {500, 300}, Qt::LeftButton, Qt::LeftButton);
        window.reset();
        QCOMPARE(releases, 1);
    }
};

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QtSessionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_qtsession.moc"
