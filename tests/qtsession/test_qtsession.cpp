#include <QtTest>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQuickItem>
#include <QQuickStyle>
#include <QScreen>
#include <QScopeGuard>
#include <QSignalSpy>
#include <fontconfig/fontconfig.h>
#include <limits>

#include "sessionwindow.h"
#include "sessionshell.h"

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

bool saveUiSnapshot(QtSessionWindow& window, const QString& name)
{
    // Optional synthetic UI evidence from the isolated hosted compositor only.
    const QString directory = qEnvironmentVariable("PLANK_QT_UI_REPORT_DIR");
    return directory.isEmpty() || window.grabWindow().save(directory + QLatin1Char('/') +
        name + QLatin1Char('-') + QString::number(window.devicePixelRatio(), 'f', 2) + QStringLiteral(".png"));
}

QImage renderedControl(QtSessionWindow& window, QQuickItem* item)
{
    const QImage rendered = window.grabWindow();
    if (rendered.isNull() || !item) return {};
    const QRectF bounds = item->mapRectToScene(QRectF(QPointF(), item->size()));
    const qreal sx = qreal(rendered.width()) / window.width();
    const qreal sy = qreal(rendered.height()) / window.height();
    return rendered.copy(QRectF(bounds.x() * sx, bounds.y() * sy,
                                bounds.width() * sx, bounds.height() * sy).toAlignedRect());
}

bool hasRenderedGlyph(const QImage& image)
{
    int brightPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() > 128 && color.green() > 128 && color.blue() > 128) ++brightPixels;
        }
    }
    // The brightest button background channel is 98. Thin vector strokes are
    // antialiased, so require clear foreground contrast rather than expecting
    // eight fully covered white pixels in an otherwise valid small glyph.
    return brightPixels >= 8;
}
}

class QtSessionTest : public QObject
{
    Q_OBJECT
private slots:
    void defaultWindowedSize_data()
    {
        QTest::addColumn<QSize>("available");
        QTest::addColumn<QSize>("desktop");
        QTest::addColumn<QSize>("expected");
        QTest::newRow("wide") << QSize(1600, 1000) << QSize(3840, 2160) << QSize(1280, 720);
        QTest::newRow("ultrawide") << QSize(1920, 1080) << QSize(5120, 2160) << QSize(1536, 648);
        QTest::newRow("dual-desktop") << QSize(1600, 1000) << QSize(7680, 2160) << QSize(1280, 360);
        QTest::newRow("portrait") << QSize(1600, 1000) << QSize(2160, 3840) << QSize(450, 800);
        QTest::newRow("square") << QSize(1600, 1000) << QSize(2160, 2160) << QSize(800, 800);
        // availableGeometry is already in logical units at fractional scaling.
        QTest::newRow("fractional-display") << QSize(1536, 864) << QSize(3840, 2160) << QSize(1228, 691);
        QTest::newRow("unknown-desktop") << QSize(1600, 1000) << QSize() << QSize(1280, 800);
        QTest::newRow("empty-desktop") << QSize(1600, 1000) << QSize(0, 0) << QSize(1280, 800);
        QTest::newRow("empty-screen") << QSize() << QSize(3840, 2160) << QSize();
    }

    void defaultWindowedSize()
    {
        QFETCH(QSize, available);
        QFETCH(QSize, desktop);
        QFETCH(QSize, expected);
        QCOMPARE(QtSessionShell::defaultWindowedSize(available, desktop), expected);
    }

    void fullscreenStartupAndWindowedRestore()
    {
        QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.wayland.debug=true"));
        const auto restoreLogging = qScopeGuard([] { QLoggingCategory::setFilterRules({}); });
        QtSessionShell shell;
        auto& scene = shell.surface();
        scene.setLiveSession(true);
        QVector<QSize> shownSizes;
        const auto trace = [&](const char* event) {
            qInfo() << event << "shell" << shell.size() << shell.windowState()
                    << shell.isVisible() << shell.isExposed()
                    << "child" << scene.size() << scene.isVisible() << scene.isExposed();
        };
        QObject observations;
        connect(&shell, &QWindow::widthChanged, &observations, [&] { trace("shell width"); });
        connect(&shell, &QWindow::heightChanged, &observations, [&] { trace("shell height"); });
        connect(&shell, &QWindow::visibilityChanged, &observations, [&] { trace("shell visibility"); });
        connect(&shell, &QWindow::windowStateChanged, &observations, [&] { trace("shell state"); });
        const QSize desktop(5120, 2160);
        const QSize expected = QtSessionShell::defaultWindowedSize(
            shell.screen()->availableGeometry().size(), desktop);
        shell.prepareForSession(desktop, true);
        trace("prepared");
        // A native backend may already apply fullscreen geometry while hidden.
        // Verify the normal restore size after leaving fullscreen, below.
        QVERIFY(!shell.isVisible());
        QVERIFY(!scene.isVisible());
        QCOMPARE(shell.windowState(), Qt::WindowFullScreen);
        connect(&scene, &QWindow::visibleChanged, &observations, [&](bool visible) {
            trace("child visibility");
            if (visible) shownSizes.append(scene.size());
        });
        shell.showFullScreen();
        trace("show requested");
        QTRY_VERIFY(scene.isExposed());
        QVERIFY(!shownSizes.isEmpty());
        qInfo() << "Initial fullscreen child geometry:" << shownSizes.first()
                << "configured shell:" << shell.size() << "state:" << shell.windowState();
        // The first child show must already use the configured fullscreen
        // geometry, not the normal restore size or the QML's 1280x720 default.
        QCOMPARE(shownSizes.first(), shell.screen()->geometry().size());
        QTRY_COMPARE(scene.size(), shell.size());
        shell.showNormal();
        QTRY_COMPARE(shell.size(), expected);
        QTRY_COMPARE(scene.size(), expected);
        QVERIFY(saveUiSnapshot(scene, QStringLiteral("windowed-default")));

        // Arbitrary manual resize remains free-form. Do not resize back to the
        // source aspect or replace this size with a fresh 80% default on exit.
        const QSize manual(900, 650);
        shell.resize(manual);
        QTRY_COMPARE(scene.size(), manual);
        for (int round = 0; round < 3; ++round) {
            shell.showFullScreen();
            QTRY_COMPARE(shell.size(), shell.screen()->geometry().size());
            QTRY_COMPARE(scene.size(), shell.size());
            shell.showNormal();
            QTRY_COMPARE(shell.size(), manual);
            QTRY_COMPARE(scene.size(), manual);
        }
        // Takeover/recovery can temporarily unmap the same shell.
        shell.hide();
        QTRY_VERIFY(!scene.isVisible());
        shell.show();
        QTRY_VERIFY(scene.isExposed());
        QCOMPARE(shell.size(), manual);
    }

    void windowedStartupUsesDesktopAspect()
    {
        QtSessionShell shell;
        shell.prepareForSession(QSize(3840, 2160), false);
        const QSize expected = QtSessionShell::defaultWindowedSize(
            shell.screen()->availableGeometry().size(), QSize(3840, 2160));
        shell.show();
        QTRY_VERIFY(shell.surface().isExposed());
        QCOMPARE(shell.windowState(), Qt::WindowNoState);
        QCOMPARE(shell.size(), expected);
        QCOMPARE(shell.surface().size(), expected);
    }

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

    void dragAcrossOutputsUsesEachViewport()
    {
        const QSize stream(5120, 2160);
        const QtSessionGeometry left{stream, {0, 0, 3840, 2160}, {0, 0, 3840, 2160},
                                     {3840, 2160}, {3072, 1728}};
        const QtSessionGeometry right{stream, {3840, 0, 1280, 2160}, {0, 0, 1280, 2160},
                                      {1280, 2160}, {1280, 2160}};
        const QVector<QtSessionOutputGeometry> outputs{{{0, 0, 3072, 1728}, left}, {{3072, 0, 1280, 2160}, right}};
        QPointF position;
        QVERIFY(qtMapDraggedPointer(outputs, {3072, 864}, position));
        QCOMPARE(position, QPointF(3840, 864));
        QVERIFY(qtMapDraggedPointer(outputs, {3712, 1080}, position));
        QCOMPARE(position, QPointF(4480, 1080));
        QVERIFY(qtMapDraggedPointer(outputs, {1536, 864}, position));
        QCOMPARE(position, QPointF(1920, 1080));
        QVERIFY(qtMapDraggedPointer(outputs, {9000, 9000}, position));
        QCOMPARE(position, QPointF(5119, 2159));
        QVERIFY(!qtMapDraggedPointer(outputs, {qQNaN(), 0}, position));
        QVERIFY(!qtMapDraggedPointer({}, {}, position));
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

    void toolbarReadoutsAreStackedAndStable()
    {
        QtSessionWindow window;
        QCOMPARE(window.status(), QQuickView::Ready);
        QVERIFY(window.rootObject());
        prepare(window);
        window.setPinned(true);
        window.setTelemetry({{"fps", 59.9}, {"videoMbps", 76.5}, {"loss", 1.55}, {"rtt", "12.3"}});
        QVERIFY(!window.grabWindow().isNull());
        const auto toolbar = window.rootObject()->findChild<QQuickItem*>("sessionToolbar");
        QVERIFY(toolbar);
        const qreal originalWidth = toolbar->width();
        const auto fullscreen = controlCenter(window, "fullscreenButton");
        const QStringList names{"fpsStat", "videoStat", "lossStat", "rttStat"};
        const QStringList labels{"FPS", "Video Mbps", "Loss", "RTT"};
        const QStringList values{"59.9", "76.5", "1.550%", "12 ms"};
        for (int i = 0; i < names.size(); ++i) {
            const auto stat = toolbar->findChild<QQuickItem*>(names[i]);
            QVERIFY(stat);
            const auto label = stat->findChild<QQuickItem*>("statLabel");
            const auto value = stat->findChild<QQuickItem*>("statValue");
            QVERIFY(label && value);
            QCOMPARE(label->property("text").toString(), labels[i]);
            QCOMPARE(value->property("text").toString(), values[i]);
            QVERIFY(value->y() >= label->y() + label->height());
            QCOMPARE(value->x(), label->x());
            QVERIFY(value->width() <= stat->width());
            QVERIFY(label->width() <= stat->width());
        }
        window.setTelemetry({{"fps", 240.0}, {"videoMbps", 150.0}, {"loss", 100.0}, {"rtt", "9999.9"}});
        window.setBitrateKbps(window.maximumBitrateKbps());
        QVERIFY(!window.grabWindow().isNull());
        QCOMPARE(toolbar->width(), originalWidth);
        QCOMPARE(controlCenter(window, "fullscreenButton"), fullscreen);
        window.setTelemetry({});
        window.setBitrateKbps(window.minimumBitrateKbps());
        QVERIFY(!window.grabWindow().isNull());
        QCOMPARE(toolbar->width(), originalWidth);
        QCOMPARE(controlCenter(window, "fullscreenButton"), fullscreen);
        QCOMPARE(toolbar->height(), 39.0);
    }

    void toolbarRttUsesCompactWholeMilliseconds()
    {
        QtSessionWindow window;
        prepare(window);
        window.setPinned(true);
        QVERIFY(!window.grabWindow().isNull());
        const auto toolbar = window.rootObject()->findChild<QQuickItem*>("sessionToolbar");
        const auto rtt = window.rootObject()->findChild<QQuickItem*>("rttStat");
        QVERIFY(toolbar && rtt);
        const auto value = rtt->findChild<QQuickItem*>("statValue");
        QVERIFY(value);
        const qreal width = toolbar->width();
        const auto slider = controlCenter(window, "bitrateSlider");
        const auto fullscreen = controlCenter(window, "fullscreenButton");
        const QList<QPair<QVariant, QString>> samples{
            {0, QStringLiteral("— ms")},
            {9, QStringLiteral("9 ms")},
            {12.6, QStringLiteral("13 ms")},
            {99, QStringLiteral("99 ms")},
            {999, QStringLiteral("999 ms")},
            {1000, QStringLiteral(">999 ms")},
            {std::numeric_limits<quint32>::max(), QStringLiteral(">999 ms")},
            // QVariant promotes signed -1 to UINT_MAX when comparing integers.
            // A real-valued negative is distinct from the previous sample.
            {-1.0, QStringLiteral("— ms")},
            {QStringLiteral("invalid"), QStringLiteral("— ms")}
        };
        for (const auto& sample : samples) {
            window.setTelemetry({{"rtt", sample.first}});
            QVERIFY(!window.grabWindow().isNull());
            QCOMPARE(value->property("text").toString(), sample.second);
            QVERIFY(value->width() <= rtt->width());
            QCOMPARE(toolbar->width(), width);
            QCOMPARE(controlCenter(window, "bitrateSlider"), slider);
            QCOMPARE(controlCenter(window, "fullscreenButton"), fullscreen);
        }
        window.setTelemetry({});
        QCOMPARE(value->property("text").toString(), QStringLiteral("— ms"));
        QVERIFY(saveUiSnapshot(window, QStringLiteral("toolbar-compact-rtt")));
    }

    void toolbarControlsHaveCompactGaps()
    {
        QtSessionWindow window;
        prepare(window);
        window.setPinned(true);
        const auto group = window.rootObject()->findChild<QQuickItem*>("toolbarControls");
        QVERIFY(group);
        // Unsupported microphone/camera controls must not leave empty slots.
        for (int supported = 0; supported < 4; ++supported) {
            const bool microphone = supported & 1;
            const bool camera = supported & 2;
            window.setTelemetry({{"microphoneSupported", microphone}, {"cameraSupported", camera}});
            QVERIFY(!window.grabWindow().isNull());
            QStringList names;
            if (microphone) names << QStringLiteral("microphoneButton");
            if (camera) names << QStringLiteral("cameraButton");
            names << QStringLiteral("fullscreenButton") << QStringLiteral("minimizeButton")
                  << QStringLiteral("disconnectButton");
            qreal previousRight = 0;
            for (const auto& name : names) {
                const auto button = group->findChild<QQuickItem*>(name);
                QVERIFY(button && button->isVisible());
                QCOMPARE(button->size(), QSizeF(32, 32));
                QCOMPARE(button->x(), previousRight);
                previousRight = button->x() + button->width() + 4;
            }
            QCOMPARE(group->width(), previousRight - 4);
        }
        QVERIFY(saveUiSnapshot(window, QStringLiteral("toolbar-compact-controls")));
    }

    void toolbarIconsSurviveRevealAndModeChanges()
    {
        QtSessionShell shell;
        auto& scene = shell.surface();
        QCOMPARE(scene.status(), QQuickView::Ready);
        QVERIFY(scene.rootObject());
        shell.resize(1280, 720);
        shell.show();
        QTRY_COMPARE(scene.size(), shell.size());
        QVERIFY(QTest::qWaitForWindowExposed(&shell));
        const QSize windowedSize = shell.size();
        scene.setPresentation(fullFrame(scene.size()));
        scene.setRemoteInputEnabled(true);
        scene.setPinned(true);
        scene.setTelemetry({{"fps", 59.9}, {"videoMbps", 76.5}, {"loss", 1.55}, {"rtt", "12.3"}});
        scene.setBitrateKbps(76500);
        const auto pin = scene.rootObject()->findChild<QQuickItem*>("pinIcon");
        const auto fullscreen = scene.rootObject()->findChild<QQuickItem*>("fullscreenIcon");
        QVERIFY(pin && fullscreen);
        // Image.Ready is 1. Validate the actual bundled image and rendered
        // pixels, including the state changes that previously left a blank box.
        QTRY_COMPARE(pin->property("status").toInt(), 1);
        QTRY_COMPARE(fullscreen->property("status").toInt(), 1);
        QCOMPARE(pin->property("source").toUrl().fileName(), QStringLiteral("pin.svg"));
        QCOMPARE(fullscreen->property("source").toUrl().fileName(), QStringLiteral("fullscreen-enter.svg"));
        const auto pinnedImage = renderedControl(scene, pin);
        const auto enterImage = renderedControl(scene, fullscreen);
        QVERIFY(saveUiSnapshot(scene, QStringLiteral("toolbar-initial-stacked")));
        QVERIFY(hasRenderedGlyph(pinnedImage));
        QVERIFY(hasRenderedGlyph(enterImage));
        QSignalSpy remote(&scene, &QtSessionWindow::remoteButton);
        const auto pinPoint = controlCenter(scene, "pinButton");
        mouse(scene, QEvent::MouseMove, pinPoint);
        mouse(scene, QEvent::MouseButtonPress, pinPoint, Qt::LeftButton, Qt::LeftButton);
        mouse(scene, QEvent::MouseButtonRelease, pinPoint, Qt::LeftButton);
        QVERIFY(!scene.pinned());
        QTRY_COMPARE(pin->property("source").toUrl().fileName(), QStringLiteral("pin-off.svg"));
        const auto unpinnedImage = renderedControl(scene, pin);
        QVERIFY(hasRenderedGlyph(unpinnedImage));
        QVERIFY(unpinnedImage != pinnedImage);
        scene.setWaiting(true);
        QVERIFY(!scene.grabWindow().isNull());
        scene.setWaiting(false);
        QVERIFY(hasRenderedGlyph(renderedControl(scene, pin)));
        scene.setPinned(true);
        QVERIFY(saveUiSnapshot(scene, QStringLiteral("toolbar-windowed-stacked")));

        const auto fullPoint = controlCenter(scene, "fullscreenButton");
        mouse(scene, QEvent::MouseButtonPress, fullPoint, Qt::LeftButton, Qt::LeftButton);
        mouse(scene, QEvent::MouseButtonRelease, fullPoint, Qt::LeftButton);
        QTRY_VERIFY(scene.fullscreen());
        QTRY_COMPARE(shell.size(), shell.screen()->geometry().size());
        QTRY_COMPARE(scene.size(), shell.size());
        QCOMPARE(scene.windowState(), Qt::WindowNoState);
        QTRY_COMPARE(fullscreen->property("source").toUrl().fileName(), QStringLiteral("fullscreen-exit.svg"));
        // State/size notifications can precede the new icon's layout and draw.
        // Require the rendered glyph, not just its changed source property.
        QTRY_VERIFY(hasRenderedGlyph(renderedControl(scene, fullscreen)));
        const auto exitImage = renderedControl(scene, fullscreen);
        QVERIFY(hasRenderedGlyph(exitImage));
        QVERIFY(exitImage != enterImage);
        QVERIFY(saveUiSnapshot(scene, QStringLiteral("toolbar-fullscreen-stacked")));

        const auto restorePoint = controlCenter(scene, "fullscreenButton");
        mouse(scene, QEvent::MouseButtonPress, restorePoint, Qt::LeftButton, Qt::LeftButton);
        mouse(scene, QEvent::MouseButtonRelease, restorePoint, Qt::LeftButton);
        QTRY_VERIFY(!scene.fullscreen());
        QTRY_COMPARE(shell.size(), windowedSize);
        QTRY_COMPARE(scene.size(), windowedSize);
        QTRY_COMPARE(fullscreen->property("source").toUrl().fileName(), QStringLiteral("fullscreen-enter.svg"));
        QVERIFY(hasRenderedGlyph(renderedControl(scene, fullscreen)));
        QVERIFY(hasRenderedGlyph(renderedControl(scene, pin)));
        QVERIFY(remote.isEmpty());
    }

    void toolbarIconsKeepTheirSizeUnderMaterial()
    {
        QCOMPARE(QQuickStyle::name(), QStringLiteral("Material"));
        QtSessionWindow window;
        QCOMPARE(window.status(), QQuickView::Ready);
        QVERIFY(window.rootObject());
        prepare(window);
        window.setPinned(true);
        window.setTelemetry({{"microphoneSupported", true}, {"microphoneState", 0},
                             {"cameraSupported", true}, {"cameraState", 0}});
        QVERIFY(!window.grabWindow().isNull());
        const auto toolbar = window.rootObject()->findChild<QQuickItem*>("sessionToolbar");
        QVERIFY(toolbar);
        mouse(window, QEvent::MouseMove, toolbar->mapToScene({1, 20}));
        for (const QString& name : {QStringLiteral("pin"), QStringLiteral("microphone"),
                                   QStringLiteral("camera"), QStringLiteral("fullscreen"),
                                   QStringLiteral("minimize"), QStringLiteral("disconnect")}) {
            const auto button = toolbar->findChild<QQuickItem*>(name + QStringLiteral("Button"));
            const auto icon = toolbar->findChild<QQuickItem*>(name + QStringLiteral("Icon"));
            QVERIFY(button && icon);
            QVERIFY(button->isVisible());
            QCOMPARE(button->size(), QSizeF(32, 32));
            QCOMPARE(icon->size(), QSizeF(24, 24));
            QCOMPARE(icon->position(), QPointF(4, 4));
            for (const char* inset : {"leftInset", "rightInset", "topInset", "bottomInset"})
                QCOMPARE(button->property(inset).toReal(), 0.0);
            const auto background = button->property("background").value<QQuickItem*>();
            QVERIFY(background);
            QCOMPARE(background->size(), button->size());
            QCOMPARE(background->property("color").value<QColor>().alpha(), 0);
            QTRY_COMPARE(icon->property("status").toInt(), 1);
            const auto pixels = renderedControl(window, icon);
            QVERIFY(hasRenderedGlyph(pixels));
            // A nonempty image is insufficient: the production Material padding
            // previously squeezed otherwise valid SVG resources to tiny marks.
            int first = pixels.width(), last = -1;
            for (int y = 0; y < pixels.height(); ++y) {
                for (int x = 0; x < pixels.width(); ++x) {
                    const auto color = pixels.pixelColor(x, y);
                    if (color.red() > 128 && color.green() > 128 && color.blue() > 128) {
                        first = qMin(first, x);
                        last = qMax(last, x);
                    }
                }
            }
            QVERIFY2(last - first + 1 >= 10 * window.devicePixelRatio(), qPrintable(name));
        }
        QVERIFY(saveUiSnapshot(window, QStringLiteral("toolbar-borderless-all-controls")));
        window.setTelemetry({{"microphoneSupported", true}, {"microphoneState", 2},
                             {"cameraSupported", true}, {"cameraState", 2}});
        const auto mic = toolbar->findChild<QQuickItem*>("microphoneIcon");
        const auto camera = toolbar->findChild<QQuickItem*>("cameraIcon");
        QVERIFY(mic && camera);
        QTRY_COMPARE(mic->property("source").toUrl().fileName(), QStringLiteral("microphone-active.svg"));
        QTRY_COMPARE(camera->property("source").toUrl().fileName(), QStringLiteral("camera-active.svg"));
        QVERIFY(saveUiSnapshot(window, QStringLiteral("toolbar-borderless-capture-active")));
    }

    void toolbarButtonsKeepInvisibleHitTargets()
    {
        QtSessionWindow window;
        QCOMPARE(window.status(), QQuickView::Ready);
        QVERIFY(window.rootObject());
        prepare(window);
        window.setLiveSession(true);
        window.setPinned(true);
        window.setTelemetry({{"microphoneSupported", true}, {"cameraSupported", true}});
        QVERIFY(!window.grabWindow().isNull());
        QSignalSpy remote(&window, &QtSessionWindow::remoteButton);
        QSignalSpy microphone(&window, &QtSessionWindow::microphoneRequested);
        QSignalSpy camera(&window, &QtSessionWindow::cameraRequested);
        QSignalSpy fullscreen(&window, &QtSessionWindow::fullscreenRequested);
        QSignalSpy minimize(&window, &QtSessionWindow::minimizeRequested);
        QSignalSpy disconnect(&window, &QtSessionWindow::disconnectRequested);
        for (const char* name : {"pinButton", "microphoneButton", "cameraButton", "fullscreenButton",
                                 "minimizeButton", "disconnectButton"}) {
            const auto button = window.rootObject()->findChild<QQuickItem*>(name);
            QVERIFY(button);
            const auto background = button->property("background").value<QQuickItem*>();
            QVERIFY(background);
            // Click outside the 24px artwork but inside its invisible 32px target.
            const auto point = button->mapToScene({1, 1});
            mouse(window, QEvent::MouseMove, point);
            QTRY_VERIFY(button->property("hovered").toBool());
            QCOMPARE(background->property("color").value<QColor>(), QColor("#444a53"));
            QVERIFY(saveUiSnapshot(window, QString::fromLatin1(name) + QStringLiteral("-hover")));
            mouse(window, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            QVERIFY(button->property("down").toBool());
            QCOMPARE(background->property("color").value<QColor>(), QColor("#525862"));
            QVERIFY(saveUiSnapshot(window, QString::fromLatin1(name) + QStringLiteral("-pressed")));
            mouse(window, QEvent::MouseButtonRelease, point, Qt::LeftButton);
            QVERIFY(!button->property("down").toBool());
        }
        QVERIFY(!window.pinned());
        QCOMPARE(microphone.count(), 1);
        QCOMPARE(camera.count(), 1);
        QCOMPARE(fullscreen.count(), 1);
        QCOMPARE(minimize.count(), 1);
        QCOMPARE(disconnect.count(), 1);
        QVERIFY(remote.isEmpty());
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

    void recoveryDialogHasOnlyDisconnectAndClearsWithoutInteraction()
    {
        QtSessionWindow window;
        QCOMPARE(window.status(), QQuickView::Ready);
        QVERIFY(window.rootObject());
        prepare(window);
        const auto overlay = window.rootObject()->findChild<QQuickItem*>("recoveryOverlay");
        QVERIFY(overlay);
        const auto disconnect = overlay->findChild<QQuickItem*>("recoveryDisconnectButton");
        QVERIFY(disconnect);
        int actions = 0;
        for (auto* item : overlay->findChildren<QQuickItem*>()) {
            if (item->metaObject()->indexOfSignal("clicked()") >= 0) ++actions;
        }
        QCOMPARE(actions, 1);
        QCOMPARE(disconnect->property("text").toString(), QStringLiteral("Disconnect"));
        QSignalSpy quit(&window, &QtSessionWindow::disconnectRequested);
        QSignalSpy remote(&window, &QtSessionWindow::remoteButton);

        window.setRemoteInputEnabled(false);
        window.setWaiting(true);
        QVERIFY(!window.grabWindow().isNull());
        QVERIFY(overlay->isVisible());
        QCOMPARE(window.cursor().shape(), Qt::ArrowCursor);
        mouse(window, QEvent::MouseButtonPress, {50, 300}, Qt::LeftButton, Qt::LeftButton);
        mouse(window, QEvent::MouseButtonRelease, {50, 300}, Qt::LeftButton);
        QVERIFY(remote.isEmpty());
        QVERIFY(quit.isEmpty());

        // Runtime completion clears the notice without any dialog action and
        // restores current input geometry before enabling remote input.
        window.setPresentation(fullFrame());
        window.setWaiting(false);
        window.setRemoteInputEnabled(true);
        QVERIFY(!window.grabWindow().isNull());
        QVERIFY(!overlay->isVisible());
        QVERIFY(quit.isEmpty());
        mouse(window, QEvent::MouseButtonPress, {50, 300}, Qt::LeftButton, Qt::LeftButton);
        mouse(window, QEvent::MouseButtonRelease, {50, 300}, Qt::LeftButton);
        QCOMPARE(remote.count(), 2);

        // A second interruption still offers an immediately usable escape.
        window.setRemoteInputEnabled(false);
        window.setWaiting(true);
        QVERIFY(!window.grabWindow().isNull());
        QVERIFY(saveUiSnapshot(window, QStringLiteral("automatic-recovery-dialog")));
        const auto point = controlCenter(window, "recoveryDisconnectButton");
        mouse(window, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        mouse(window, QEvent::MouseButtonRelease, point, Qt::LeftButton);
        QCOMPARE(quit.count(), 1);
        QCOMPARE(remote.count(), 2);
    }

    void tabletCursorTextureReplacement()
    {
        QtSessionWindow window;
        prepare(window);
        QTRY_VERIFY(window.isExposed());
        window.setTabletActive(true);
        window.setHostCursorPosition({1500, 900});

        const auto cursorPixel = [&window] {
            // Host (1500, 900) maps to logical (500, 300). Sample inside the
            // cursor, not on an antialiased edge, at any backing-pixel scale.
            const QImage rendered = window.grabWindow();
            if (rendered.isNull()) return QColor();
            return rendered.pixelColor(qRound(505.0 * rendered.width() / window.width()),
                                       qRound(305.0 * rendered.height() / window.height()));
        };
        const QColor background = cursorPixel();
        QVERIFY(background.isValid());

        // Render every replacement, rather than coalescing all shapes before
        // the first frame. The old implementation double-deleted the first
        // rendered texture when setTexture() replaced it on the next frame.
        for (int i = 0; i < 24; ++i) {
            const QColor color = i % 2 ? Qt::green : Qt::red;
            QImage shape(i % 2 ? 90 : 60, 60, QImage::Format_ARGB32_Premultiplied);
            shape.fill(color);
            window.setCursorShape(shape, {3, 3}, true);
            QCOMPARE(cursorPixel(), color);
        }

        // Clear/recreate and mouse/tablet ownership changes must also retire
        // the old scene-graph node safely, then show the latest Host shape.
        window.setCursorShape({}, {}, false);
        QCOMPARE(cursorPixel(), background);
        QImage shape(60, 60, QImage::Format_ARGB32_Premultiplied);
        shape.fill(Qt::blue);
        window.setCursorShape(shape, {}, true);
        QCOMPARE(cursorPixel(), QColor(Qt::blue));
        window.setTabletActive(false);
        QCOMPARE(cursorPixel(), background);
        shape.fill(Qt::red);
        window.setCursorShape(shape, {}, true);
        window.setTabletActive(true);
        window.setHostCursorPosition({1500, 900});
        QCOMPARE(cursorPixel(), QColor(Qt::red));
        // Window destruction exercises final texture retirement too.
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
        // Leave the wider windowed reveal strip.
        mouse(window, QEvent::MouseMove, {1000, 20});
        QTest::qWait(700);
        QVERIFY(!window.toolbarVisible());
        mouse(window, QEvent::MouseMove, {1270, 0});
        QTRY_VERIFY_WITH_TIMEOUT(window.toolbarVisible(), 1600);
        // No further motion is necessary for the five-second hide timeout.
        QTRY_VERIFY_WITH_TIMEOUT(!window.toolbarVisible(), 6000);
    }

    void windowedEdgeAllowsEasierDwell()
    {
        QtSessionShell shell;
        auto& scene = shell.surface();
        shell.resize(1280, 720);
        shell.show();
        QTRY_COMPARE(scene.size(), shell.size());
        QTRY_VERIFY(scene.isExposed());
        scene.setPresentation(fullFrame(scene.size()));
        scene.setRemoteInputEnabled(true);
        // Twelve logical pixels below the title bar, independent of DPI and
        // Host geometry. Leaving that strip must still cancel its timer.
        mouse(scene, QEvent::MouseMove, {20, 11});
        QTest::qWait(400);
        mouse(scene, QEvent::MouseMove, {20, 12});
        QTest::qWait(700);
        QVERIFY(!scene.toolbarVisible());
        mouse(scene, QEvent::MouseMove, {20, 11});
        QTRY_VERIFY_WITH_TIMEOUT(scene.toolbarVisible(), 1600);
        QVERIFY(!scene.pinned());

        // A fresh fullscreen shell keeps the narrow physical-edge trigger.
        QtSessionShell fullscreenShell;
        fullscreenShell.resize(1280, 720);
        fullscreenShell.showFullScreen();
        auto& fullscreenScene = fullscreenShell.surface();
        QTRY_VERIFY(fullscreenScene.fullscreen());
        QTRY_VERIFY(fullscreenScene.isExposed());
        fullscreenScene.setPresentation(fullFrame(fullscreenScene.size()));
        fullscreenScene.setRemoteInputEnabled(true);
        mouse(fullscreenScene, QEvent::MouseMove, {20, 11});
        QTest::qWait(1100);
        QVERIFY(!fullscreenScene.toolbarVisible());
        mouse(fullscreenScene, QEvent::MouseMove, {20, 2});
        QTRY_VERIFY_WITH_TIMEOUT(fullscreenScene.toolbarVisible(), 1600);
    }

    void windowedRevealTracksShellStateAndRecovery()
    {
        QtSessionShell shell;
        auto& scene = shell.surface();
        shell.resize(1280, 720);
        shell.show();
        QTRY_COMPARE(scene.size(), shell.size());
        QTRY_VERIFY(scene.isExposed());
        const QSize windowedSize = shell.size();
        for (int round = 0; round < 3; ++round) {
            shell.showFullScreen();
            QTRY_VERIFY(scene.fullscreen());
            // The requested Qt state changes before the compositor confirms
            // the resize. Complete each native transition before injecting
            // the next input sequence, just as a user would see it.
            QTRY_COMPARE(shell.size(), shell.screen()->geometry().size());
            QTRY_COMPARE(scene.size(), shell.size());
            // The scene remains a native child; only the shell is fullscreen.
            QCOMPARE(scene.windowState(), Qt::WindowNoState);
            shell.showNormal();
            QTRY_VERIFY(!scene.fullscreen());
            QTRY_COMPARE(shell.size(), windowedSize);
            QTRY_COMPARE(scene.size(), windowedSize);
        }
        // Hover works without keyboard activation. The isolated compositor has
        // no physical input seat to activate this shell, so wait for exposure
        // and an actual frame, not keyboard focus.
        QVERIFY(QTest::qWaitForWindowExposed(&shell));
        QVERIFY(!scene.grabWindow().isNull());
        QCoreApplication::processEvents();
        scene.setPresentation(fullFrame(scene.size()));
        scene.setRemoteInputEnabled(true);
        scene.setWaiting(true);
        mouse(scene, QEvent::MouseMove, {20, 8});
        QTest::qWait(1100);
        QVERIFY(!scene.toolbarVisible());
        scene.setWaiting(false);
        QVERIFY(!scene.grabWindow().isNull());
        QCoreApplication::processEvents();
        mouse(scene, QEvent::MouseMove, {20, 8});
        QTRY_VERIFY_WITH_TIMEOUT(scene.toolbarVisible(), 1600);
        QVERIFY(!scene.pinned());
        QVERIFY(saveUiSnapshot(scene, QStringLiteral("windowed-toolbar-revealed")));
        scene.setPinned(true);
        shell.showFullScreen();
        shell.showNormal();
        QTRY_VERIFY(!scene.fullscreen());
        QVERIFY(scene.pinned());
        QVERIFY(scene.toolbarVisible());
    }

    void windowedRevealPreservesDragAndReleasedInput()
    {
        QtSessionWindow scene;
        prepare(scene);
        QSignalSpy buttons(&scene, &QtSessionWindow::remoteButton);
        mouse(scene, QEvent::MouseButtonPress, {500, 300}, Qt::LeftButton, Qt::LeftButton);
        mouse(scene, QEvent::MouseMove, {20, 8}, Qt::NoButton, Qt::LeftButton);
        QTest::qWait(1100);
        QVERIFY(!scene.toolbarVisible());
        mouse(scene, QEvent::MouseButtonRelease, {20, 8}, Qt::LeftButton);
        QCOMPARE(buttons.count(), 2);
        QVERIFY(!buttons.last().at(1).toBool());
        buttons.clear();

        // Released-input mode must still allow reveal and local fullscreen.
        scene.setLiveSession(true);
        scene.setRemoteInputEnabled(false);
        QSignalSpy recapture(&scene, &QtSessionWindow::inputRecaptureRequested);
        QSignalSpy fullscreen(&scene, &QtSessionWindow::fullscreenRequested);
        mouse(scene, QEvent::MouseMove, {20, 8});
        QTRY_VERIFY_WITH_TIMEOUT(scene.toolbarVisible(), 1600);
        const auto full = controlCenter(scene, "fullscreenButton");
        mouse(scene, QEvent::MouseButtonPress, full, Qt::LeftButton, Qt::LeftButton);
        mouse(scene, QEvent::MouseButtonRelease, full, Qt::LeftButton);
        QCOMPARE(fullscreen.count(), 1);
        QVERIFY(recapture.isEmpty());
        QVERIFY(buttons.isEmpty());
        QVERIFY(!scene.pinned());
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

    void shellOwnsWindowStateAndGeometry()
    {
        QtSessionShell shell;
        auto& scene = shell.surface();
        shell.resize(1280, 720);
        shell.show();
        QTRY_COMPARE(scene.size(), shell.size());
        QTRY_VERIFY(scene.isExposed());
        QVERIFY(!scene.isTopLevel());
        QCOMPARE(scene.parent(), &shell);
        scene.setPinned(true);
        scene.setRemoteInputEnabled(true);
        scene.setPresentation(fullFrame(scene.size()));
        QSignalSpy buttons(&scene, &QtSessionWindow::remoteButton);
        mouse(scene, QEvent::MouseButtonPress, {500, 300}, Qt::LeftButton, Qt::LeftButton);
        shell.resize(1100, 680);
        QTRY_COMPARE(scene.size(), QSize(1100, 680));
        // Geometry properties update before native resize-event delivery.
        QTRY_COMPARE(buttons.count(), 2);

        for (int round = 0; round < 3; ++round) {
            const auto button = controlCenter(scene, "fullscreenButton");
            mouse(scene, QEvent::MouseButtonPress, button, Qt::LeftButton, Qt::LeftButton);
            mouse(scene, QEvent::MouseButtonRelease, button, Qt::LeftButton);
            QTRY_COMPARE(shell.windowState(), Qt::WindowFullScreen);
            QTRY_COMPARE(scene.size(), shell.size());
            QCOMPARE(scene.windowState(), Qt::WindowNoState);
            scene.toggleFullscreen();
            QTRY_COMPARE(shell.windowState(), Qt::WindowNoState);
            QTRY_COMPARE(scene.size(), shell.size());
        }
        scene.minimize();
        QTRY_COMPARE(shell.windowState(), Qt::WindowMinimized);
        QVERIFY(!scene.isVisible());
        shell.showNormal();
        QTRY_COMPARE(shell.windowState(), Qt::WindowNoState);
        QTRY_VERIFY(scene.isVisible());
        shell.hide();
        QVERIFY(!scene.isVisible());
        shell.showNormal();
        QTRY_VERIFY(scene.isVisible());
        QSignalSpy disconnect(&scene, &QtSessionWindow::disconnectRequested);
        shell.close();
        QCOMPARE(disconnect.count(), 1);
    }

    void shellCancelsInputAndRoutesKeyboard()
    {
        QtSessionShell shell;
        auto& scene = shell.surface();
        shell.resize(1280, 720);
        shell.show();
        QTRY_COMPARE(scene.size(), shell.size());
        scene.setPresentation(fullFrame(scene.size()));
        scene.setRemoteInputEnabled(true);
        QSignalSpy buttons(&scene, &QtSessionWindow::remoteButton);
        QSignalSpy keys(&scene, &QtSessionWindow::remoteKey);
        mouse(scene, QEvent::MouseButtonPress, {500, 300}, Qt::LeftButton, Qt::LeftButton);
        QKeyEvent down(QEvent::KeyPress, Qt::Key_A, {}, 38, 0, 0);
        QCoreApplication::sendEvent(&shell, &down);
        QCOMPARE(keys.count(), 1);
        QEvent inactive(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&shell, &inactive);
        QCOMPARE(buttons.count(), 2);
        QCOMPARE(keys.count(), 2);
        QVERIFY(!keys.last().at(2).toBool());
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_A, {}, 38, 0, 0);
        QCoreApplication::sendEvent(&shell, &up);
        QCOMPARE(keys.count(), 2);
        scene.setWaiting(true);
        QCoreApplication::sendEvent(&shell, &down);
        QCOMPARE(keys.count(), 2);
    }

    void coveredShellKeepsRenderingAndRestoresAfterHide()
    {
        if (QGuiApplication::platformName() != QStringLiteral("wayland"))
            QSKIP("Requires the real isolated Wayland compositor");
        QtSessionShell shell;
        auto& scene = shell.surface();
        scene.setLiveSession(true);
        shell.resize(1280, 720);
        QSignalSpy swaps(&scene, &QQuickWindow::frameSwapped);
        for (bool fullscreen : {false, true, false, true}) {
            if (fullscreen) shell.showFullScreen();
            else shell.showNormal();
            QTRY_VERIFY(shell.isExposed());
            QTRY_VERIFY(scene.isExposed());
            // Survive the parent's missing-callback timeout, then require new
            // actual swaps. Merely showing a window or grabbing it can hide
            // this regression by rendering before the timeout fires.
            QTest::qWait(400);
            for (int frame = 0; frame < 8; ++frame) {
                const int before = swaps.count();
                scene.setStatusText(QString::number(frame));
                scene.update();
                QTRY_VERIFY_WITH_TIMEOUT(swaps.count() > before, 1000);
                QVERIFY(scene.isExposed());
            }
            shell.hide();
            QTRY_VERIFY(!shell.isExposed());
            QTRY_VERIFY(!scene.isExposed());
        }
        shell.showNormal();
        QTRY_VERIFY(scene.isExposed());
        shell.showMinimized();
        QTRY_COMPARE(shell.windowState(), Qt::WindowMinimized);
        QTRY_VERIFY(!scene.isVisible());
        QTRY_VERIFY(!scene.isExposed());
        shell.showNormal();
        QTRY_VERIFY(scene.isExposed());
        const int before = swaps.count();
        scene.update();
        QTRY_VERIFY_WITH_TIMEOUT(swaps.count() > before, 1000);
    }

    void releasedInputCanBeRecapturedWithoutAClickLeak()
    {
        QtSessionWindow scene;
        prepare(scene);
        scene.setLiveSession(true);
        scene.setRemoteInputEnabled(false);
        QSignalSpy recapture(&scene, &QtSessionWindow::inputRecaptureRequested);
        QSignalSpy buttons(&scene, &QtSessionWindow::remoteButton);
        connect(&scene, &QtSessionWindow::inputRecaptureRequested, &scene, [&scene] { scene.setRemoteInputEnabled(true); });
        mouse(scene, QEvent::MouseButtonPress, {500, 300}, Qt::LeftButton, Qt::LeftButton);
        mouse(scene, QEvent::MouseButtonRelease, {500, 300}, Qt::LeftButton);
        QCOMPARE(recapture.count(), 1); QVERIFY(buttons.isEmpty());
        scene.setRemoteInputEnabled(false);
        QKeyEvent resume(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
        QCoreApplication::sendEvent(&scene, &resume);
        QCOMPARE(recapture.count(), 2);
        scene.setRemoteInputEnabled(false); scene.setWaiting(true);
        QCoreApplication::sendEvent(&scene, &resume);
        QCOMPARE(recapture.count(), 2);
    }

    void liveWindowControlsAreSessionWideRequests()
    {
        QtSessionShell shell;
        shell.surface().setLiveSession(true);
        QSignalSpy fullscreen(&shell.surface(), &QtSessionWindow::fullscreenRequested);
        QSignalSpy minimize(&shell.surface(), &QtSessionWindow::minimizeRequested);
        shell.surface().toggleFullscreen(); shell.surface().minimize();
        QCOMPARE(fullscreen.count(), 1); QCOMPARE(minimize.count(), 1);
        QCOMPARE(shell.windowState(), Qt::WindowNoState);
    }

    void disconnectWithoutRemoteInput_data()
    {
        QTest::addColumn<bool>("waiting");
        QTest::addColumn<bool>("remoteEnabled");
        QTest::addColumn<int>("key");
        QTest::newRow("startup-or-released") << false << false << int(Qt::Key_Q);
        QTest::newRow("waiting-released") << true << false << int(Qt::Key_Q);
        QTest::newRow("waiting-enabled") << true << true << int(Qt::Key_Q);
        QTest::newRow("non-latin-layout") << true << false << 0x419;
    }

    void disconnectWithoutRemoteInput()
    {
        QFETCH(bool, waiting);
        QFETCH(bool, remoteEnabled);
        QFETCH(int, key);
        QtSessionShell shell;
        auto& scene = shell.surface();
        scene.setLiveSession(true);
        scene.setRemoteInputEnabled(remoteEnabled);
        scene.setWaiting(waiting);
        QSignalSpy disconnect(&scene, &QtSessionWindow::disconnectRequested);
        QSignalSpy keys(&scene, &QtSessionWindow::remoteKey);
        constexpr auto modifiers = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
        QKeyEvent ordinary(QEvent::KeyPress, key, Qt::ControlModifier, 24, 0, 0);
        QCoreApplication::sendEvent(&shell, &ordinary);
        QVERIFY(disconnect.isEmpty());
        QKeyEvent repeat(QEvent::KeyPress, key, modifiers, 24, 0, 0, {}, true);
        QCoreApplication::sendEvent(&shell, &repeat);
        QVERIFY(disconnect.isEmpty());
        QKeyEvent down(QEvent::KeyPress, key, modifiers, 24, 0, 0);
        QCoreApplication::sendEvent(&shell, &down);
        QCOMPARE(disconnect.count(), 1);
        QKeyEvent up(QEvent::KeyRelease, key, modifiers, 24, 0, 0);
        QCoreApplication::sendEvent(&shell, &up);
        QCOMPARE(disconnect.count(), 1);
        QVERIFY(keys.isEmpty());
    }
};

int main(int argc, char** argv)
{
    // Material initializes Fontconfig's process-wide alias/configuration cache.
    // Finalize it only after all test scenes and QGuiApplication are destroyed;
    // keep leak detection enabled rather than suppressing library allocations.
    const auto fontconfigCleanup = qScopeGuard([] { FcFini(); });
    // Match the production launcher. Default-style-only qualification missed
    // the Material edge padding that squeezed compact toolbar icons.
    qputenv("QT_QUICK_CONTROLS_MATERIAL_THEME", "Dark");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_VARIANT", "Dense");
    qputenv("QT_QUICK_CONTROLS_MATERIAL_ACCENT", "#5A9CE6");
    QQuickStyle::setStyle(QStringLiteral("Material"));
    qputenv("QSG_RENDER_LOOP", "basic");
    QtSessionShell::configureWaylandExposure();
    QGuiApplication app(argc, argv);
    QtSessionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_qtsession.moc"
