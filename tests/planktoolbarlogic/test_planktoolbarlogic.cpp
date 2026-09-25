#include <QtTest>

#include "planktoolbarlogic.h"
#include "planktoolbarstats.h"
#include "input/plankpointerlogic.h"
#include "videopacketlosswindow.h"

using PlankToolbarLogic::ButtonRouter;

class TestPlankToolbarLogic : public QObject
{
    Q_OBJECT

private slots:
    void preservesCenteredAndDraggedPositions();
    void resolvesReportedAndDerivedDensity();
    void alignsLogicalHitRectWithPhysicalSurface_data();
    void alignsLogicalHitRectWithPhysicalSurface();
    void preservesHorizontalPositionAcrossScaleChanges();
    void isolatesVisibleNativeChildFromParentMotion();
    void keepsNativeButtonSequenceOutOfSdlPath();
    void keepsRemotePressReleaseTogether();
    void keepsLocalPressReleaseTogether();
    void keepsMultiButtonSequenceWithFirstOwner();
    void confinesToVideoWithoutToolbar();
    void reachesWindowEdgeWithToolbar();
    void calculatesOnePreFecLossInterval();
    void resetsPreFecLossAfterCounterRestart();
    void rejectsInconsistentFecCounters();
    void retainsTenSecondPeakForBothStatsViews();
    void sharesPacketLossDisplayPrecision();
    void formatsSharedNetworkRtt();
    void sizesEncoderSegmentWithoutMovingSlider();
};

void TestPlankToolbarLogic::preservesCenteredAndDraggedPositions()
{
    using namespace PlankToolbarLogic;
    for (int windowWidth : {400, 855, 1710, 2560, 3840}) {
        const int toolbarWidth = std::min(windowWidth, 539);
        const int available = windowWidth - toolbarWidth;
        // Centered reveal, either edge, and an operator-chosen position all
        // survive a layout refresh. There is no camera exclusion horizontally.
        for (int left : {0, available / 2, available * 3 / 4, available}) {
            const float position = horizontalPosition(left, windowWidth, toolbarWidth);
            QCOMPARE(logicalLeftFromPosition(position, windowWidth, toolbarWidth), left);
        }
        QVERIFY(std::abs(logicalLeftFromPosition(0.5f, windowWidth, toolbarWidth) -
                         available / 2) <= 1);
        QCOMPARE(logicalLeftFromPosition(-1.0f, windowWidth, toolbarWidth), 0);
        QCOMPARE(logicalLeftFromPosition(2.0f, windowWidth, toolbarWidth), available);
    }
}

void TestPlankToolbarLogic::resolvesReportedAndDerivedDensity()
{
    QCOMPARE(PlankToolbarLogic::resolvePixelDensity(
                 1.25f, 4096, 1728, 5120, 2160), 1.25f);
    QCOMPARE(PlankToolbarLogic::resolvePixelDensity(
                 0.0f, 4096, 1728, 5120, 2160), 1.25f);
    QCOMPARE(PlankToolbarLogic::resolvePixelDensity(
                 0.0f, 0, 0, 0, 0), 1.0f);
}

void TestPlankToolbarLogic::alignsLogicalHitRectWithPhysicalSurface_data()
{
    QTest::addColumn<float>("density");
    QTest::newRow("100-percent") << 1.0f;
    QTest::newRow("125-percent") << 1.25f;
    QTest::newRow("150-percent") << 1.5f;
    QTest::newRow("200-percent") << 2.0f;
}

void TestPlankToolbarLogic::alignsLogicalHitRectWithPhysicalSurface()
{
    QFETCH(float, density);

    constexpr int logicalWindowWidth = 4096;
    constexpr int logicalToolbarWidth = 539;
    constexpr int logicalLeft = 2671;
    const int pixelWindowWidth = PlankToolbarLogic::physicalExtent(
                logicalWindowWidth, density);
    const int pixelToolbarWidth = PlankToolbarLogic::physicalExtent(
                logicalToolbarWidth, density);
    const float position = PlankToolbarLogic::horizontalPosition(
                logicalLeft, logicalWindowWidth, logicalToolbarWidth);
    const int renderedPixelLeft = qRound(
                position * (pixelWindowWidth - pixelToolbarWidth));
    const int expectedPixelLeft = qRound(logicalLeft * density);

    QVERIFY(qAbs(renderedPixelLeft - expectedPixelLeft) <= 1);
    QVERIFY(qAbs((renderedPixelLeft + pixelToolbarWidth) -
                 qRound((logicalLeft + logicalToolbarWidth) * density)) <= 1);
}

void TestPlankToolbarLogic::preservesHorizontalPositionAcrossScaleChanges()
{
    constexpr int toolbarWidth = 539;
    constexpr int oldWindowWidth = 3840;
    constexpr int oldLeft = 2476;
    constexpr int newWindowWidth = 3072;
    const float position = PlankToolbarLogic::horizontalPosition(
                oldLeft, oldWindowWidth, toolbarWidth);
    const int newLeft = PlankToolbarLogic::logicalLeftFromPosition(
                position, newWindowWidth, toolbarWidth);

    QVERIFY(qAbs(PlankToolbarLogic::horizontalPosition(
                     newLeft, newWindowWidth, toolbarWidth) - position) < 0.001f);
}

void TestPlankToolbarLogic::isolatesVisibleNativeChildFromParentMotion()
{
    QVERIFY(!PlankToolbarLogic::nativeChildOwnsPointerSequence(
                false, true, false));
    QVERIFY(!PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, false, false));
    QVERIFY(PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, true, false));
}

void TestPlankToolbarLogic::keepsNativeButtonSequenceOutOfSdlPath()
{
    QVERIFY(!PlankToolbarLogic::nativeChildOwnsPointerSequence(
                false, true, true));
    QVERIFY(!PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, false, false));
    QVERIFY(PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, true, false));
    QVERIFY(PlankToolbarLogic::nativeChildOwnsPointerSequence(
                true, false, true));
}

void TestPlankToolbarLogic::keepsRemotePressReleaseTogether()
{
    ButtonRouter router;
    QCOMPARE(router.routeButton(1, true, false), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeMotion(true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeButton(1, false, true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeMotion(true), ButtonRouter::Owner::Local);
}

void TestPlankToolbarLogic::keepsLocalPressReleaseTogether()
{
    ButtonRouter router;
    QCOMPARE(router.routeButton(1, true, true), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeMotion(false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeButton(1, false, false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeMotion(false), ButtonRouter::Owner::Remote);
}

void TestPlankToolbarLogic::keepsMultiButtonSequenceWithFirstOwner()
{
    ButtonRouter router;
    QCOMPARE(router.routeButton(1, true, true), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeButton(3, true, false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeButton(1, false, false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeMotion(false), ButtonRouter::Owner::Local);
    QCOMPARE(router.routeButton(3, false, false), ButtonRouter::Owner::Local);

    QCOMPARE(router.routeButton(1, true, false), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeButton(3, true, true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeButton(1, false, true), ButtonRouter::Owner::Remote);
    QCOMPARE(router.routeButton(3, false, true), ButtonRouter::Owner::Remote);
}

void TestPlankToolbarLogic::confinesToVideoWithoutToolbar()
{
    const PlankPointerLogic::Rect window = {0, 0, 3840, 2160};
    const PlankPointerLogic::Rect video = {0, 228, 3840, 1704};

    const auto rect = PlankPointerLogic::pointerConfinementRect(
                window, video, false);

    QCOMPARE(rect.x, 0);
    QCOMPARE(rect.y, 228);
    QCOMPARE(rect.w, 3840);
    QCOMPARE(rect.h, 1704);
}

void TestPlankToolbarLogic::reachesWindowEdgeWithToolbar()
{
    const PlankPointerLogic::Rect window = {0, 0, 3840, 2160};
    const PlankPointerLogic::Rect video = {0, 228, 3840, 1704};

    const auto rect = PlankPointerLogic::pointerConfinementRect(
                window, video, true);

    QCOMPARE(rect.x, 0);
    QCOMPARE(rect.y, 0);
    QCOMPARE(rect.w, 3840);
    QCOMPARE(rect.h, 2160);
}

void TestPlankToolbarLogic::calculatesOnePreFecLossInterval()
{
    VideoPacketLossInterval interval;
    QVERIFY(!interval.addCumulative(1000, 10, 2).has_value());

    const auto clean = interval.addCumulative(2000, 10, 2);
    QVERIFY(clean.has_value());
    QCOMPARE(clean->before, 0.0f);
    QCOMPARE(clean->after, 0.0f);

    const auto loss = interval.addCumulative(3000, 35, 2);
    QVERIFY(loss.has_value());
    QCOMPARE(loss->before, 2.5f);
    QCOMPARE(loss->after, 0.0f); // All missing originals reconstructed.
    const auto residual = interval.addCumulative(4000, 75, 12);
    QVERIFY(residual.has_value());
    QCOMPARE(residual->before, 4.0f);
    QCOMPARE(residual->after, 1.0f);
}

void TestPlankToolbarLogic::resetsPreFecLossAfterCounterRestart()
{
    VideoPacketLossInterval interval;
    QVERIFY(!interval.addCumulative(1000, 10, 2).has_value());
    QVERIFY(interval.addCumulative(2000, 20, 3).has_value());
    QVERIFY(!interval.addCumulative(50, 1, 0).has_value());

    const auto restarted = interval.addCumulative(150, 3, 1);
    QVERIFY(restarted.has_value());
    QCOMPARE(restarted->before, 2.0f);
    QCOMPARE(restarted->after, 1.0f);
    QVERIFY(!interval.addCumulative(100, 101, 0).has_value());
}

void TestPlankToolbarLogic::rejectsInconsistentFecCounters()
{
    VideoPacketLossInterval interval;
    QVERIFY(!interval.addCumulative(100, 10, 11).has_value());
    QVERIFY(!interval.addCumulative(100, 10, 2).has_value());
    QVERIFY(!interval.addCumulative(100, 10, 2).has_value());
    // Cumulative values look valid, but the interval is not coherent.
    QVERIFY(!interval.addCumulative(200, 11, 4).has_value());
    const auto valid = interval.addCumulative(300, 21, 6);
    QVERIFY(valid.has_value());
    QCOMPARE(valid->before, 10.0f);
    QCOMPARE(valid->after, 2.0f);
    // A reset in only one component establishes a fresh baseline.
    QVERIFY(!interval.addCumulative(400, 22, 0).has_value());
}

void TestPlankToolbarLogic::retainsTenSecondPeakForBothStatsViews()
{
    VideoPacketLossPeakWindow window;
    QCOMPARE(window.addSample(1000, 0.5f), 0.5f);
    QCOMPARE(window.addSample(5000, 4.0f), 4.0f);
    QCOMPARE(window.addSample(10999, 0.0f), 4.0f);
    QCOMPARE(window.addSample(15000, 1.0f), 1.0f);
    window.reset();
    QCOMPARE(window.addSample(15001, 0.0f), 0.0f);
}

void TestPlankToolbarLogic::sharesPacketLossDisplayPrecision()
{
    QCOMPARE(VideoPacketLossDisplayDecimalPlaces, 2);
}

void TestPlankToolbarLogic::formatsSharedNetworkRtt()
{
    QCOMPARE(PlankToolbarStats::networkRttText(0), QStringLiteral("—"));
    QCOMPARE(PlankToolbarStats::networkRttText(1), QStringLiteral("1 ms"));
    QCOMPARE(PlankToolbarStats::networkRttText(12), QStringLiteral("12 ms"));
    QCOMPARE(PlankToolbarStats::networkRttText(150), QStringLiteral("150 ms"));
    QCOMPARE(PlankToolbarStats::networkRttText(1234), QStringLiteral("1234 ms"));
}

void TestPlankToolbarLogic::sizesEncoderSegmentWithoutMovingSlider()
{
    using namespace PlankToolbarStats;
    const int width = encoderTargetWidth(10000, 150000, 500);
    const QFontMetrics metrics(encoderTargetFont());
    int widest = 0;
    for (int target = 10000; target <= 150000; target += 500) {
        const int textWidth = metrics.horizontalAdvance(encoderTargetText(target));
        QVERIFY(textWidth <= width);
        widest = std::max(widest, textWidth);
    }
    QCOMPARE(width, widest);
    QCOMPARE(encoderTargetText(42500), QStringLiteral("Encoder target  42.5 Mbps"));
    QVERIFY(EncoderTargetLeft > RttLeft + RttWidth);
    const int toolbarWidth = EncoderTargetLeft + width + WindowControlsWidth;
    const int sliderRight = toolbarWidth - WindowControlsWidth;
    QCOMPARE(sliderRight - EncoderTargetLeft, width);
    // The knob's seven-point hit extension must stop before Fullscreen.
    QVERIFY(sliderRight + 7 < toolbarWidth - 102);
}

QTEST_MAIN(TestPlankToolbarLogic)

#include "test_planktoolbarlogic.moc"
