#include <QtTest>

#include "streaming/plankpresentation.h"
#include "streaming/input/plankmousemotion.h"
#include "streaming/input/plankpointerlogic.h"

class TestPlankPresentation : public QObject
{
    Q_OBJECT

private slots:
    void rejectsStaleTabletFocus();
    void coalescesAdjacentMouseMotion();
    void preservesMouseMotionBarriers_data();
    void preservesMouseMotionBarriers();
    void exactDualOutputSlices();
    void manualHostModesOverrideRetinaPanelBoundary();
    void letterboxedDualOutputSlices();
    void mapsEachWindowIntoOneStreamCanvas();
    void preservesMappingWithScaledLogicalWindows();
    void mapsCursorIntoSingleOutput();
    void mapsCursorAcrossDualOutputSeam();
    void mapsCursorAcrossAsymmetricOutputSeam();
    void mapsMixedRetinaDrawable();
    void scalesLetterboxToBackingPixels();
    void rejectsInvisibleOrInvalidDrawables();
    void routesCapturedDragAcrossMixedDpiOutputs();
    void routesCapturedDragAtExactSeam();
    void retainsCapturedDragOutsidePresentation();
    void rejectsInvalidPointerSource();
};

void TestPlankPresentation::rejectsStaleTabletFocus()
{
    using PlankPointerLogic::tabletFocusPositionIsCurrent;
    QVERIFY(tabletFocusPositionIsCurrent(true, true, 101, 100));
    // Returning to the real mouse cancels tablet authority, even if a newer
    // Host packet was already queued. Resuming needs a post-activation sample.
    QVERIFY(!tabletFocusPositionIsCurrent(false, true, 101, 100));
    QVERIFY(!tabletFocusPositionIsCurrent(true, false, 101, 100));
    QVERIFY(!tabletFocusPositionIsCurrent(true, true, 100, 100));
    QVERIFY(!tabletFocusPositionIsCurrent(true, true, 99, 100));
    // Reconnect resets the position epoch; no sample is eligible until received.
    QVERIFY(!tabletFocusPositionIsCurrent(true, false, 0, 0));
    QVERIFY(tabletFocusPositionIsCurrent(true, true, 1, 0));
}

void TestPlankPresentation::exactDualOutputSlices()
{
    const QSize size(5120, 2160);
    const auto left = PlankPresentation::sliceForOutput(
                size, size, QRect(0, 0, 2560, 2160));
    const auto right = PlankPresentation::sliceForOutput(
                size, size, QRect(2560, 0, 2560, 2160));

    QVERIFY(left.visible);
    QCOMPARE(left.sourceRect, QRectF(0, 0, 2560, 2160));
    QCOMPARE(left.destinationRect, QRect(0, 0, 2560, 2160));
    QVERIFY(right.visible);
    QCOMPARE(right.sourceRect, QRectF(2560, 0, 2560, 2160));
    QCOMPARE(right.destinationRect, QRect(0, 0, 2560, 2160));
}

void TestPlankPresentation::manualHostModesOverrideRetinaPanelBoundary()
{
    const QSize stream(4480, 1440);
    const auto canvas = PlankPresentation::horizontalCanvas(
        {QSize(1920, 1200), QSize(2560, 1440)});
    QCOMPARE(canvas.size(), 2);
    QCOMPARE(canvas[0], QRect(0, 0, 1920, 1200));
    QCOMPARE(canvas[1], QRect(1920, 0, 2560, 1440));

    const auto laptop = PlankPresentation::sliceForDrawable(
        stream, stream, canvas[0], QSize(4112, 2572));
    const auto external = PlankPresentation::sliceForDrawable(
        stream, stream, canvas[1], QSize(2560, 1440));
    QCOMPARE(laptop.sourceRect, QRectF(0, 0, 1920, 1200));
    QCOMPARE(external.sourceRect, QRectF(1920, 0, 2560, 1440));
    QCOMPARE(laptop.destinationRect, QRect(0, 0, 4112, 2572));
    QCOMPARE(external.destinationRect, QRect(0, 0, 2560, 1440));
    QVERIFY(PlankPresentation::horizontalCanvas({QSize(1920, 1200), QSize()}).isEmpty());
}

void TestPlankPresentation::letterboxedDualOutputSlices()
{
    const QSize stream(3840, 2160);
    const QSize canvas(5120, 2160);
    QCOMPARE(PlankPresentation::videoRect(stream, canvas),
             QRect(640, 0, 3840, 2160));

    const auto left = PlankPresentation::sliceForOutput(
                stream, canvas, QRect(0, 0, 2560, 2160));
    const auto right = PlankPresentation::sliceForOutput(
                stream, canvas, QRect(2560, 0, 2560, 2160));
    QCOMPARE(left.sourceRect, QRectF(0, 0, 1920, 2160));
    QCOMPARE(left.destinationRect, QRect(640, 0, 1920, 2160));
    QCOMPARE(right.sourceRect, QRectF(1920, 0, 1920, 2160));
    QCOMPARE(right.destinationRect, QRect(0, 0, 1920, 2160));
}

void TestPlankPresentation::mapsEachWindowIntoOneStreamCanvas()
{
    QPointF point;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(1280, 1080), QSize(2560, 2160),
                QSize(5120, 2160), QSize(5120, 2160),
                QRect(0, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(1280, 1080));

    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(0, 1080), QSize(2560, 2160),
                QSize(5120, 2160), QSize(5120, 2160),
                QRect(2560, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(2560, 1080));
}

void TestPlankPresentation::preservesMappingWithScaledLogicalWindows()
{
    QPointF point;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
                QPointF(1024, 864), QSize(2048, 1728),
                QSize(5120, 2160), QSize(5120, 2160),
                QRect(2560, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(3840, 1080));

    QPointF windowPoint;
    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                point, QSize(5120, 2160), QSize(5120, 2160),
                QRect(2560, 0, 2560, 2160), QSize(2048, 1728),
                windowPoint));
    QCOMPARE(windowPoint, QPointF(1024, 864));
}

void TestPlankPresentation::mapsCursorIntoSingleOutput()
{
    QPointF windowPoint;
    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(960, 540), QSize(1920, 1080), QSize(1920, 1080),
                QRect(0, 0, 1920, 1080), QSize(1920, 1080), windowPoint));
    QCOMPARE(windowPoint, QPointF(960, 540));
}

void TestPlankPresentation::mapsCursorAcrossDualOutputSeam()
{
    const QSize canvas(5120, 2160);
    QPointF windowPoint;

    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(2559, 1080), canvas, canvas,
                QRect(0, 0, 2560, 2160), QSize(2560, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(2559, 1080));
    QVERIFY(!PlankPresentation::mapStreamPointToWindow(
                QPointF(2560, 1080), canvas, canvas,
                QRect(0, 0, 2560, 2160), QSize(2560, 2160), windowPoint));

    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(2560, 1080), canvas, canvas,
                QRect(2560, 0, 2560, 2160), QSize(2560, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(0, 1080));
}

void TestPlankPresentation::mapsCursorAcrossAsymmetricOutputSeam()
{
    const QSize canvas(5120, 2160);
    QPointF windowPoint;

    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(3839, 1080), canvas, canvas,
                QRect(0, 0, 3840, 2160), QSize(3840, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(3839, 1080));
    QVERIFY(!PlankPresentation::mapStreamPointToWindow(
                QPointF(3840, 1080), canvas, canvas,
                QRect(0, 0, 3840, 2160), QSize(3840, 2160), windowPoint));

    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(3840, 1080), canvas, canvas,
                QRect(3840, 0, 1280, 2160), QSize(1280, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(0, 1080));
    QVERIFY(PlankPresentation::mapStreamPointToWindow(
                QPointF(5119, 1080), canvas, canvas,
                QRect(3840, 0, 1280, 2160), QSize(1280, 2160), windowPoint));
    QCOMPARE(windowPoint, QPointF(1279, 1080));
}

void TestPlankPresentation::mapsMixedRetinaDrawable()
{
    const QSize canvas(6016, 2234);
    const QRect laptop(2560, 0, 3456, 2234);
    const auto slice = PlankPresentation::sliceForDrawable(canvas, canvas, laptop,
                                                          QSize(3456, 2234));
    QCOMPARE(slice.sourceRect, QRectF(laptop));
    QCOMPARE(slice.destinationRect, QRect(0, 0, 3456, 2234));
    QPointF streamPoint, windowPoint;
    QVERIFY(PlankPresentation::mapWindowPointToStream(
        QPointF(864, 558.5), QSize(1728, 1117), canvas, canvas, laptop,
        streamPoint, false));
    QCOMPARE(streamPoint, QPointF(4288, 1117));
    QVERIFY(PlankPresentation::mapStreamPointToWindow(streamPoint, canvas, canvas,
        laptop, QSize(1728, 1117), windowPoint));
    QCOMPARE(windowPoint, QPointF(864, 558.5));
}

void TestPlankPresentation::scalesLetterboxToBackingPixels()
{
    const auto slice = PlankPresentation::sliceForDrawable(
        QSize(3840, 2160), QSize(5120, 2160), QRect(0, 0, 2560, 2160),
        QSize(1280, 1080));
    QVERIFY(slice.visible);
    QCOMPARE(slice.sourceRect, QRectF(0, 0, 1920, 2160));
    QCOMPARE(slice.destinationRect, QRect(320, 0, 960, 1080));
    QPointF point;
    QVERIFY(!PlankPresentation::mapWindowPointToStream(
        QPointF(100, 500), QSize(1280, 1080), QSize(3840, 2160),
        QSize(5120, 2160), QRect(0, 0, 2560, 2160), point, false));
    QVERIFY(PlankPresentation::mapWindowPointToStream(
        QPointF(800, 540), QSize(1280, 1080), QSize(3840, 2160),
        QSize(5120, 2160), QRect(0, 0, 2560, 2160), point, false));
    QCOMPARE(point, QPointF(960, 1080));
}

void TestPlankPresentation::rejectsInvisibleOrInvalidDrawables()
{
    QVERIFY(!PlankPresentation::sliceForDrawable(QSize(1920, 2160),
        QSize(5120, 2160), QRect(0, 0, 1280, 2160), QSize(1280, 2160)).visible);
    QVERIFY(!PlankPresentation::sliceForDrawable(QSize(1920, 1080),
        QSize(1920, 1080), QRect(), QSize(1920, 1080)).visible);
    QVERIFY(!PlankPresentation::sliceForDrawable(QSize(1920, 1080),
        QSize(1920, 1080), QRect(0, 0, 1920, 1080), QSize(0, 0)).visible);
}

void TestPlankPresentation::routesCapturedDragAcrossMixedDpiOutputs()
{
    // The button remains captured by the originating window in both directions.
    // Logical desktop coordinates use the active Mac scaling, not panel pixels.
    const QVector<QRect> windows {QRect(-2056, 0, 2056, 1329),
                                 QRect(0, 0, 2560, 1440)};
    const QVector<QRect> canvasRects {QRect(0, 0, 3456, 2234),
                                     QRect(3456, 0, 2560, 1440)};
    const QSize canvas(6016, 2234);
    QPointF local, stream;
    int target = PlankPresentation::resolvePointerOutput(windows, 1,
                                                         QPointF(-1028, 664.5), local);
    QCOMPARE(target, 0);
    QCOMPARE(local, QPointF(1028, 664.5));
    QVERIFY(PlankPresentation::mapWindowPointToStream(local, windows[target].size(),
        canvas, canvas, canvasRects[target], stream, true));
    QCOMPARE(stream, QPointF(1728, 1117));

    target = PlankPresentation::resolvePointerOutput(windows, 0,
                                                      QPointF(3336, 720), local);
    QCOMPARE(target, 1);
    QCOMPARE(local, QPointF(1280, 720));
    QVERIFY(PlankPresentation::mapWindowPointToStream(local, windows[target].size(),
        canvas, canvas, canvasRects[target], stream, true));
    QCOMPARE(stream, QPointF(4736, 720));
}

void TestPlankPresentation::routesCapturedDragAtExactSeam()
{
    const QVector<QRect> windows {QRect(-1728, 40, 1728, 1117),
                                 QRect(0, 0, 2560, 1440)};
    QPointF local;
    QCOMPARE(PlankPresentation::resolvePointerOutput(windows, 0,
        QPointF(1728, 160), local), 1);
    QCOMPARE(local, QPointF(0, 200));
    QCOMPARE(PlankPresentation::resolvePointerOutput(windows, 1,
        QPointF(-0.5, 200), local), 0);
    QCOMPARE(local, QPointF(1727.5, 160));
}

void TestPlankPresentation::retainsCapturedDragOutsidePresentation()
{
    const QVector<QRect> windows {QRect(-1728, 0, 1728, 1117),
                                 QRect(0, 0, 2560, 1440)};
    QPointF local;
    // Below the shorter screen: preserve the existing clamping/release policy.
    QCOMPARE(PlankPresentation::resolvePointerOutput(windows, 1,
        QPointF(-100, 1200), local), 1);
    QCOMPARE(local, QPointF(-100, 1200));
    QCOMPARE(PlankPresentation::resolvePointerOutput(windows, 1,
        QPointF(100, 200), local), 1);
    QCOMPARE(local, QPointF(100, 200));
}

void TestPlankPresentation::rejectsInvalidPointerSource()
{
    QPointF local;
    QCOMPARE(PlankPresentation::resolvePointerOutput({}, 0, QPointF(), local), -1);
    QCOMPARE(PlankPresentation::resolvePointerOutput({QRect()}, 0, QPointF(), local), -1);
    QCOMPARE(PlankPresentation::resolvePointerOutput({QRect(0, 0, 100, 100)},
        1, QPointF(), local), -1);
}

namespace {
struct EventQueue {
    bool initialized = SDL_InitSubSystem(SDL_INIT_EVENTS);
    ~EventQueue() { if (initialized) SDL_QuitSubSystem(SDL_INIT_EVENTS); }
};
SDL_Event mouseMotion(float x)
{
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = 1;
    e.motion.which = 1;
    e.motion.x = x;
    e.motion.xrel = 2;
    return e;
}
}

void TestPlankPresentation::coalescesAdjacentMouseMotion()
{
    EventQueue queue;
    QVERIFY(queue.initialized);
    auto first = mouseMotion(10), second = mouseMotion(20), third = mouseMotion(30);
    QVERIFY(SDL_PushEvent(&second));
    QVERIFY(SDL_PushEvent(&third));
    PlankMouseMotion::coalescePending(first.motion);
    QCOMPARE(first.motion.x, 30.0f);
    QCOMPARE(first.motion.xrel, 6.0f);
    QVERIFY(!SDL_HasEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST));
}

void TestPlankPresentation::preservesMouseMotionBarriers_data()
{
    QTest::addColumn<int>("barrier");
    QTest::newRow("press") << 0;
    QTest::newRow("release") << 1;
    QTest::newRow("key") << 2;
    QTest::newRow("wheel") << 3;
    QTest::newRow("focus") << 4;
    QTest::newRow("other-window") << 5;
    QTest::newRow("synthetic-touch") << 6;
    QTest::newRow("button-state") << 7;
}

void TestPlankPresentation::preservesMouseMotionBarriers()
{
    QFETCH(int, barrier);
    EventQueue queue;
    QVERIFY(queue.initialized);
    auto first = mouseMotion(10), adjacent = mouseMotion(20);
    auto boundary = mouseMotion(30), later = mouseMotion(99);
    switch (barrier) {
    case 0: boundary.type = SDL_EVENT_MOUSE_BUTTON_DOWN; break;
    case 1: boundary.type = SDL_EVENT_MOUSE_BUTTON_UP; break;
    case 2: boundary.type = SDL_EVENT_KEY_DOWN; break;
    case 3: boundary.type = SDL_EVENT_MOUSE_WHEEL; break;
    case 4: boundary.type = SDL_EVENT_WINDOW_FOCUS_LOST; break;
    case 5: boundary.motion.windowID = 2; break;
    case 6: boundary.motion.which = SDL_TOUCH_MOUSEID; break;
    case 7: boundary.motion.state = SDL_BUTTON_LMASK; break;
    }
    QVERIFY(SDL_PushEvent(&adjacent));
    QVERIFY(SDL_PushEvent(&boundary));
    QVERIFY(SDL_PushEvent(&later));
    PlankMouseMotion::coalescePending(first.motion);
    QCOMPARE(first.motion.x, 20.0f);
    QCOMPARE(first.motion.xrel, 4.0f);
    SDL_Event remaining[2];
    QCOMPARE(SDL_PeepEvents(remaining, 2, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST), 2);
    QCOMPARE(remaining[0].type, boundary.type);
    QCOMPARE(remaining[0].motion.windowID, boundary.motion.windowID);
    QCOMPARE(remaining[0].motion.which, boundary.motion.which);
    QCOMPARE(remaining[0].motion.state, boundary.motion.state);
    QCOMPARE(remaining[1].motion.x, 99.0f);
}

QTEST_APPLESS_MAIN(TestPlankPresentation)

#include "test_plankpresentation.moc"
