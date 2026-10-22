#include <QtTest>
#include <QGuiApplication>
#include <QSignalSpy>
#include "../../app/streaming/qt/sessioninput.h"
#include "../../app/streaming/input/linuxrawwacom.h"
#include "../../app/streaming/input/linuxwacom.h"
#include <mutex>
#include <thread>
#include <utility>

// Link-time test doubles only. The production runtime uses the unchanged
// readers; these tests require no physical device or transport connection.
namespace {
struct Wire { int kind, a, b, c, d; };
QList<Wire> wire;
int positionResult = 0;
std::mutex tabletMutex;
QStringList tabletCalls;
std::function<void()> tabletActivityHandler;
bool tabletUsedGui = false;
bool normalized = false;
void tabletCall(const QString& call)
{
    std::lock_guard<std::mutex> lock(tabletMutex);
    tabletUsedGui |= QThread::currentThread() == qGuiApp->thread();
    tabletCalls.append(call);
}
bool saw(const QString& call)
{
    std::lock_guard<std::mutex> lock(tabletMutex);
    return tabletCalls.contains(call);
}
void registerTabletActivity(std::function<void()> activity)
{
    std::lock_guard<std::mutex> lock(tabletMutex);
    tabletActivityHandler = std::move(activity);
}
struct Fixture {
    QtSessionInput input;
    Fixture() { input.setStreamSize({5120, 2160}); input.setEnabled(true); input.setFocused(true); }
    ~Fixture() { std::thread([this] { input.shutdown(); }).join(); }
    void start(unsigned flags) { std::thread([&] { input.startTablets(flags); }).join(); }
};
}
extern "C" int LiSendMousePositionEvent(short x, short y, short w, short h)
{ wire.append({0, x, y, w, h}); return positionResult; }
extern "C" int LiSendMouseButtonEvent(char action, int button)
{ wire.append({1, action, button, 0, 0}); return 0; }
extern "C" int LiSendKeyboardEvent2(short key, char action, char modifiers, char flags)
{ wire.append({2, static_cast<unsigned short>(key) & 0xff, action, modifiers, flags}); return 0; }
extern "C" int LiSendHighResScrollEvent(short amount) { wire.append({3, amount, 0, 0, 0}); return 0; }
extern "C" int LiSendHighResHScrollEvent(short amount) { wire.append({4, amount, 0, 0, 0}); return 0; }
extern "C" int LiSendUtf8TextEvent(const char*, unsigned int) { return 0; }

PlankWacomTransportDecision plankWacomTransportForConnectedDevice()
{ return {normalized ? PlankWacomTransport::NormalizedPen : PlankWacomTransport::ExactRawHid, 0, 0}; }
LinuxRawWacomInput::LinuxRawWacomInput(std::function<void()> activity)
{ registerTabletActivity(std::move(activity)); tabletCall("raw-create"); }
LinuxRawWacomInput::~LinuxRawWacomInput() { registerTabletActivity({}); tabletCall("raw-destroy"); }
void LinuxRawWacomInput::setActive(bool active) { tabletCall(active ? "raw-active" : "raw-inactive"); }
void LinuxRawWacomInput::beginReconnect() { tabletCall("raw-suspend"); }
void LinuxRawWacomInput::finishReconnect() { tabletCall("raw-resume"); }
void LinuxRawWacomInput::handleControl(const unsigned char*, unsigned int) {}
LinuxWacomInput::LinuxWacomInput(std::function<void()> activity)
{ registerTabletActivity(std::move(activity)); tabletCall("pen-create"); }
LinuxWacomInput::~LinuxWacomInput() { registerTabletActivity({}); tabletCall("pen-destroy"); }
void LinuxWacomInput::setActive(bool active) { tabletCall(active ? "pen-active" : "pen-inactive"); }
void LinuxWacomInput::beginReconnect() { tabletCall("pen-suspend"); }
void LinuxWacomInput::finishReconnect() { tabletCall("pen-resume"); }

class QtInputRuntimeTest : public QObject
{
    Q_OBJECT
private slots:
    void init()
    {
        wire.clear();
        positionResult = 0;
        std::lock_guard<std::mutex> lock(tabletMutex);
        tabletCalls.clear(); tabletUsedGui = false; normalized = false;
        tabletActivityHandler = {};
        qunsetenv("PLANK_EXTERNAL_WACOM_BRIDGE");
    }
    void mouseOrderingAndFocusRelease()
    {
        Fixture f;
        f.input.mouseButton(Qt::LeftButton, true, {5000, 2100});
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);
        QCOMPARE(wire[0].a, 5000); QCOMPARE(wire[0].c, 5120);
        QCOMPARE(wire[1].kind, 1); QCOMPARE(wire[1].a, int(BUTTON_ACTION_PRESS));
        f.input.setFocused(false);
        QCOMPARE(wire.size(), 3); QCOMPARE(wire.last().a, int(BUTTON_ACTION_RELEASE));
        f.input.mouseButton(Qt::LeftButton, false, {0, 0});
        QCOMPARE(wire.size(), 3);
        f.input.setEnabled(false);
        f.input.mouseButton(Qt::LeftButton, true, {0, 0});
        QCOMPARE(wire.size(), 3);
    }
    void physicalModifiersRepeatAndRelease()
    {
        Fixture f;
        f.input.key(KEY_LEFTSHIFT + 8, Qt::Key_Shift, true, false, Qt::ShiftModifier);
        f.input.key(KEY_A + 8, Qt::Key_A, true, false, Qt::ShiftModifier);
        f.input.key(KEY_A + 8, Qt::Key_A, true, true, Qt::ShiftModifier);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].a, 0xa0);
        QCOMPARE(wire[1].a, 0x41); QCOMPARE(wire[1].c, int(MODIFIER_SHIFT));
        f.input.setEnabled(false);
        QCOMPARE(wire.size(), 4);
        QCOMPARE(wire[2].b, int(KEY_ACTION_UP)); QCOMPARE(wire[3].b, int(KEY_ACTION_UP));
        f.input.key(KEY_A + 8, Qt::Key_A, false, false, {});
        QCOMPARE(wire.size(), 4);
    }
    void shortcutsStayLocalAndWheelUnitsStayExact()
    {
        Fixture f;
        QSignalSpy shortcut(&f.input, &QtSessionInput::shortcutRequested);
        f.input.key(KEY_Q + 8, Qt::Key_Q, true, false, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
        f.input.key(KEY_Q + 8, Qt::Key_Q, false, false, {});
        QCOMPARE(shortcut.size(), 1); QCOMPARE(shortcut[0][0].toInt(), int(Qt::Key_Q));
        QVERIFY(wire.isEmpty());
        f.input.mouseMotion({100, 200});
        wire.clear();
        f.input.wheel({100, 200}, {-30, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 3); QCOMPARE(wire[0].a, 120);
        QCOMPARE(wire[1].kind, 4); QCOMPARE(wire[1].a, -30);
    }
    void stationaryWheelDoesNotInjectMotion()
    {
        Fixture f;
        f.input.mouseMotion({100.1, 200.1});
        wire.clear();
        for (const int delta : {120, 120, -120, -30, -30, -30, -30, 240})
            f.input.wheel({100.2, 200.2}, {0, delta}, {}, false);
        QCOMPARE(wire.size(), 8);
        for (const auto& event : wire) QCOMPARE(event.kind, 3);
        QCOMPARE(wire[3].a, -30); QCOMPARE(wire.last().a, 240);

        // Zero-delta phase events must not disturb the Host's pointer source.
        wire.clear();
        f.input.wheel({300, 400}, {}, {}, false);
        QVERIFY(wire.isEmpty());
    }
    void wheelPositionsOnlyWhenNeeded()
    {
        Fixture f;
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[1].kind, 3);
        wire.clear();
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 1); QCOMPARE(wire[0].kind, 3);

        // Another viewport may deliver a wheel without an intervening motion.
        wire.clear();
        f.input.wheel({5000, 2000}, {120, 0}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[0].a, 5000);
        QCOMPARE(wire[0].b, 2000); QCOMPARE(wire[1].kind, 4);

        // Button and real motion behavior stays unchanged; their last position
        // also satisfies a subsequent wheel's positioning requirement.
        wire.clear();
        f.input.mouseButton(Qt::LeftButton, true, {5000, 2000});
        f.input.mouseButton(Qt::LeftButton, false, {5000, 2000});
        QCOMPARE(wire.size(), 3); QCOMPARE(wire[0].kind, 0);
        f.input.mouseMotion({5000, 2000});
        QCOMPARE(wire.size(), 4); QCOMPARE(wire.last().kind, 0);
        f.input.wheel({5000, 2000}, {0, -120}, {}, false);
        QCOMPARE(wire.size(), 5); QCOMPARE(wire.last().kind, 3);
    }
    void wheelRepositionsAfterInputAndStreamChanges()
    {
        Fixture f;
        f.input.mouseMotion({100, 200});
        wire.clear();
        f.input.setFocused(false); f.input.setFocused(true);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);

        wire.clear();
        f.input.setEnabled(false);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QVERIFY(wire.isEmpty());
        f.input.setEnabled(true);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);

        wire.clear();
        f.input.setStreamSize({3840, 2160});
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[0].c, 3840);

        wire.clear();
        QSignalSpy shape(&f.input, &QtSessionInput::cursorShape);
        std::thread([&] { f.input.resetCursorEpoch(); }).join();
        QTRY_COMPARE(shape.size(), 1);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);

        wire.clear();
        f.input.setStreamSize({0, 0});
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QVERIFY(wire.isEmpty());
        f.input.setStreamSize({3840, 2160});
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0);
    }
    void wheelRetriesFailedPosition()
    {
        Fixture f;
        f.input.mouseMotion({100, 200});
        positionResult = -1;
        f.input.mouseMotion({300, 400});
        wire.clear();
        f.input.wheel({300, 400}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 1); QCOMPARE(wire[0].kind, 0);
        wire.clear();
        positionResult = 0;
        f.input.wheel({300, 400}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[1].kind, 3);
        wire.clear();
        f.input.wheel({300, 400}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 1); QCOMPARE(wire[0].kind, 3);
    }
    void wheelRepositionsAfterTablet_data()
    {
        QTest::addColumn<bool>("pen");
        QTest::addColumn<bool>("pending");
        QTest::newRow("raw-active") << false << false;
        QTest::newRow("raw-pending") << false << true;
        QTest::newRow("normalized-active") << true << false;
        QTest::newRow("normalized-pending") << true << true;
    }
    void wheelRepositionsAfterTablet()
    {
        QFETCH(bool, pen);
        QFETCH(bool, pending);
        normalized = pen;
        Fixture f;
        f.start(LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND | LI_FF_PEN_TOUCH_EVENTS);
        QTRY_VERIFY(saw(pen ? "pen-active" : "raw-active"));
        std::function<void()> activity;
        { std::lock_guard<std::mutex> lock(tabletMutex); activity = tabletActivityHandler; }
        QVERIFY(activity);
        f.input.mouseMotion({100, 200});
        wire.clear();
        QSignalSpy active(&f.input, &QtSessionInput::tabletActive);
        activity();
        if (!pending) QTRY_COMPARE(active.size(), 1);
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 2); QCOMPARE(wire[0].kind, 0); QCOMPARE(wire[1].kind, 3);
        if (!pending) { QCOMPARE(active.size(), 2); QVERIFY(!active.last()[0].toBool()); }
        wire.clear();
        QCoreApplication::processEvents();
        f.input.wheel({100, 200}, {0, 120}, {}, false);
        QCOMPARE(wire.size(), 1); QCOMPARE(wire[0].kind, 3);
    }
    void tabletLifecycle_data()
    {
        QTest::addColumn<bool>("pen");
        QTest::newRow("descriptor-driven-raw") << false;
        QTest::newRow("normalized-first-generation") << true;
    }
    void tabletLifecycle()
    {
        QFETCH(bool, pen);
        normalized = pen;
        const auto prefix = pen ? QStringLiteral("pen-") : QStringLiteral("raw-");
        {
            Fixture f;
            f.start(LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND | LI_FF_PEN_TOUCH_EVENTS);
            QTRY_VERIFY(saw(prefix + "active"));
            f.input.setFocused(false);
            QTRY_VERIFY(saw(prefix + "inactive"));
            std::thread([&] { f.input.beginReconnect(); f.input.resetCursorEpoch(); f.input.finishReconnect(); }).join();
            QVERIFY(saw(prefix + "suspend")); QVERIFY(saw(prefix + "resume"));
            f.input.setFocused(true);
        }
        QVERIFY(saw(prefix + "destroy"));
        std::lock_guard<std::mutex> lock(tabletMutex);
        QVERIFY(!tabletUsedGui);
        QCOMPARE(tabletCalls.count(prefix + "create"), 1);
        QCOMPARE(tabletCalls.count(prefix + "destroy"), 1);
    }
    void externalTabletBridgeDoesNotOpenASecondReader()
    {
        qputenv("PLANK_EXTERNAL_WACOM_BRIDGE", "1");
        { Fixture f; f.start(LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND | LI_FF_PEN_TOUCH_EVENTS); }
        std::lock_guard<std::mutex> lock(tabletMutex);
        QVERIFY(tabletCalls.isEmpty());
    }
};
QTEST_MAIN(QtInputRuntimeTest)
#include "test_qtinputruntime.moc"
