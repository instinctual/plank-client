// Exercise the production tap callback/queue with synthetic CGEvents. These
// tests never install a system tap, request permission or post OS input.
#include "../../app/streaming/mackeyboardcapture.mm"
#include <QtTest>
#include <vector>

class TestMacKeyboardCapture : public QObject
{
    Q_OBJECT
    bool m_Focus = true;
    bool m_CaptureEnabled = true;
    int m_Releases = 0;
    std::unique_ptr<MacKeyboardCapture> m_Capture;

    MacKeyboardCapture::State& state() { return *m_Capture->m_State; }
    CGEventRef key(unsigned raw, CGEventType type = kCGEventKeyDown, CGEventFlags flags = 0)
    {
        auto event = CGEventCreateKeyboardEvent(nullptr, raw, type == kCGEventKeyDown);
        CGEventSetType(event, type);
        CGEventSetFlags(event, flags);
        return event;
    }
    bool capture(unsigned raw, CGEventType type = kCGEventKeyDown, CGEventFlags flags = 0)
    {
        auto event = key(raw, type, flags);
        bool consumed = state().handle(type, event) == nullptr;
        CFRelease(event);
        return consumed;
    }
    std::vector<MacKeyEvent> drain()
    {
        std::vector<MacKeyEvent> result;
        m_Capture->dispatchPending([&](const MacKeyEvent& event) { result.push_back(event); });
        return result;
    }

private slots:
    void init()
    {
        m_Focus = true;
        m_CaptureEnabled = true;
        m_Releases = 0;
        m_Capture.reset(new MacKeyboardCapture([this] { return m_Focus && m_CaptureEnabled; },
                                               [this] { ++m_Releases; }, false));
        m_Capture->setQueueWake([] { return true; });
        state().active = true;
        state().isoKeyboard = false;
        state().isTrusted = [] { return false; };
    }
    void cleanup() { m_Capture.reset(); }

    void disabledCaptureDoesNotPromptOrConsumeRequest()
    {
        bool prompted = false;
        QVERIFY(!accessibilityPromptNeeded(false, false, prompted));
        QVERIFY(!prompted);
        QVERIFY(accessibilityPromptNeeded(true, false, prompted));
    }
    void grantedPermissionDoesNotPrompt()
    {
        bool prompted = false;
        QVERIFY(!accessibilityPromptNeeded(true, true, prompted));
        QVERIFY(!prompted);
    }
    void launcherRequestsOnlyOncePerProcess()
    {
        bool prompted = false;
        QVERIFY(accessibilityPromptNeeded(true, false, prompted));
        QVERIFY(prompted);
        QVERIFY(!accessibilityPromptNeeded(true, false, prompted));
        QVERIFY(!accessibilityPromptNeeded(false, false, prompted));
        QVERIFY(!accessibilityPromptNeeded(true, false, prompted));
    }

    void commandTabAndSpaceAreQueuedOnce()
    {
        const auto flags = kCGEventFlagMaskCommand | NX_DEVICELCMDKEYMASK;
        QVERIFY(capture(kVK_Command, kCGEventFlagsChanged, flags));
        QVERIFY(capture(kVK_Tab, kCGEventKeyDown, flags));
        QVERIFY(capture(kVK_Tab, kCGEventKeyUp, flags));
        QVERIFY(capture(kVK_Space, kCGEventKeyDown, flags));
        QVERIFY(capture(kVK_Space, kCGEventKeyUp, flags));
        QVERIFY(capture(kVK_Command, kCGEventFlagsChanged));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(6));
        QCOMPARE(keys[0].code, kVK_Command);
        QVERIFY(keys[0].down);
        QCOMPARE(keys[1].code, kVK_Tab);
        QVERIFY(keys[1].modifiers & Qt::MetaModifier);
        QVERIFY(!keys[2].down);
        QCOMPARE(keys[3].code, kVK_Space);
        QVERIFY(!keys[4].down);
        QVERIFY(!keys[5].down);
        QVERIFY(drain().empty());
        QVERIFY(m_Capture->capturesKeys());
    }
    void qtDispatcherCoalescesWakeEvents()
    {
        state().active = false;
        int wakes = 0;
        m_Capture->setQueueWake([&] { ++wakes; return true; });
        state().active = true;
        QVERIFY(capture(kVK_ANSI_A));
        QVERIFY(capture(kVK_ANSI_A, kCGEventKeyUp));
        QCOMPARE(wakes, 1);
        std::vector<MacKeyEvent> keys;
        m_Capture->dispatchPending([&](const MacKeyEvent& event) { keys.push_back(event); });
        QCOMPARE(keys.size(), size_t(2));
        QVERIFY(keys[0].down && !keys[1].down);
        QVERIFY(capture(kVK_ANSI_B));
        QCOMPARE(wakes, 2);
        m_Focus = false;
        m_Capture->dispatchPending([&](const MacKeyEvent& event) { keys.push_back(event); });
        QCOMPARE(keys.size(), size_t(2)); // Background keys cannot leak to Host.
        QVERIFY(state().pending.empty());
    }
    void qtDispatcherFailureReleasesCapture()
    {
        state().active = false;
        m_Capture->setQueueWake([] { return false; });
        state().active = true;
        QVERIFY(!capture(kVK_ANSI_A));
        QVERIFY(!state().active);
        QVERIFY(state().pending.empty());
        QCOMPARE(m_Releases, 1);
    }
    void heldAndRightModifiersAreReconciled()
    {
        const auto flags = kCGEventFlagMaskCommand | NX_DEVICERCMDKEYMASK |
                           kCGEventFlagMaskShift | NX_DEVICERSHIFTKEYMASK;
        QVERIFY(capture(kVK_Tab, kCGEventKeyDown, flags));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(3));
        QCOMPARE(keys[0].code, kVK_RightShift);
        QCOMPARE(keys[1].code, kVK_RightCommand);
        QCOMPARE(keys[2].code, kVK_Tab);
        QVERIFY(capture(kVK_RightCommand, kCGEventFlagsChanged));
        keys = drain();
        QCOMPARE(keys.size(), size_t(2));
        QVERIFY(!keys[0].down && !keys[1].down);
    }
    void aggregateModifiersAreNotDuplicated()
    {
        QVERIFY(capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(2));
        QCOMPARE(keys[0].code, kVK_Command);
        QCOMPARE(keys[1].modifiers, Qt::KeyboardModifiers(Qt::MetaModifier));
    }
    void quitAndReleaseShortcutStayOnExistingInputPath()
    {
        QVERIFY(capture(kVK_ANSI_Q, kCGEventKeyDown, kCGEventFlagMaskCommand));
        auto keys = drain();
        QCOMPARE(keys.back().code, kVK_ANSI_Q);
        QVERIFY(capture(kVK_ANSI_Z, kCGEventKeyDown, kCGEventFlagMaskControl |
                        kCGEventFlagMaskAlternate | kCGEventFlagMaskShift));
        keys = drain();
        QCOMPARE(keys.back().code, kVK_ANSI_Z);
        QVERIFY(keys.back().modifiers & Qt::ControlModifier);
        QVERIFY(keys.back().modifiers & Qt::AltModifier);
        QVERIFY(keys.back().modifiers & Qt::ShiftModifier);
    }
    void repeatsDoNotFillQueue()
    {
        QVERIFY(capture(kVK_ANSI_A));
        auto event = key(kVK_ANSI_A);
        CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, 1);
        for (int i = 0; i < 1000; ++i) QVERIFY(state().handle(kCGEventKeyDown, event) == nullptr);
        CFRelease(event);
        QCOMPARE(drain().size(), size_t(1));
        QVERIFY(capture(kVK_ANSI_A, kCGEventKeyUp));
        QVERIFY(!drain().back().down);
    }
    void focusLossDiscardsQueuedKeys()
    {
        QVERIFY(capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
        m_Focus = false;
        QVERIFY(drain().empty());
        QCOMPARE(m_Releases, 1);
        QVERIFY(!state().active);
        QVERIFY(!capture(kVK_Space));
        QVERIFY(!m_Capture->capturesKeys());
    }
    void focusIsCheckedInTapBeforeQueuedQtFocusNotification()
    {
        m_Focus = false;
        QVERIFY(!capture(kVK_ANSI_A));
        QCOMPARE(m_Releases, 1);
        QVERIFY(state().pending.empty());
    }
    void repeatedSpaceReturnsCaptureFirstShortcutWithoutRefresh()
    {
        state().isTrusted = [] { return true; };
        for (int i = 0; i < 5; ++i) {
            QVERIFY(capture(kVK_ANSI_A)); // Pending input must not cross Spaces.
            m_Focus = false;
            m_Capture->refresh();
            QVERIFY(!state().active);
            QVERIFY(drain().empty());
            QVERIFY(!capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
            QVERIFY(!capture(kVK_Tab, kCGEventKeyUp, kCGEventFlagMaskCommand));
            QVERIFY(drain().empty());

            // No Qt notification, explicit refresh or timer on return.
            m_Focus = true;
            QVERIFY(capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
            QVERIFY(capture(kVK_Tab, kCGEventKeyUp, kCGEventFlagMaskCommand));
            QVERIFY(capture(kVK_Command, kCGEventFlagsChanged));
            const auto keys = drain();
            QCOMPARE(keys.size(), size_t(4));
            QCOMPARE(keys[0].code, kVK_Command);
            QCOMPARE(keys[1].code, kVK_Tab);
            QVERIFY(keys[1].down && !keys[2].down && !keys[3].down);
            QVERIFY(m_Capture->capturesKeys());
        }
        QCOMPARE(m_Releases, 10); // One release on departure and on re-arm.
    }
    void focusReturnDoesNotOverrideExplicitCaptureRelease()
    {
        state().isTrusted = [] { return true; };
        m_CaptureEnabled = false;
        m_Capture->refresh();
        m_Focus = false;
        QVERIFY(!capture(kVK_ANSI_A));
        m_Focus = true;
        QVERIFY(!capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
        m_CaptureEnabled = true;
        QVERIFY(capture(kVK_Space, kCGEventKeyDown, kCGEventFlagMaskCommand));
        QCOMPARE(drain().back().code, kVK_Space);
    }
    void focusReturnDoesNotOverridePermissionRevocation()
    {
        m_Focus = false;
        m_Capture->refresh();
        m_Focus = true;
        QVERIFY(!capture(kVK_Tab, kCGEventKeyDown, kCGEventFlagMaskCommand));
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
    }
    void backgroundKeysNeverEnterTheRemoteQueue()
    {
        state().isTrusted = [] { return true; };
        m_Focus = false;
        for (int i = 0; i < 10; ++i) {
            QVERIFY(!capture(kVK_ANSI_A));
            QVERIFY(!capture(kVK_Command, kCGEventFlagsChanged, kCGEventFlagMaskCommand));
        }
        QCOMPARE(m_Releases, 1);
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
        QVERIFY(!m_Capture->capturesKeys());
    }
    void releaseDuringDispatchDropsRemainingKeys()
    {
        QVERIFY(capture(kVK_ANSI_A));
        QVERIFY(capture(kVK_ANSI_B));
        int delivered = 0;
        m_Capture->dispatchPending([&](const MacKeyEvent&) { ++delivered; state().deactivate(); });
        QCOMPARE(delivered, 1);
        QCOMPARE(m_Releases, 1);
        QVERIFY(state().pending.empty());
    }
    void deniedOrRevokedPermissionReleasesCapture()
    {
        QVERIFY(capture(kVK_ANSI_A));
        m_Capture->refresh(); // Injected denied permission, no OS prompt.
        QCOMPARE(m_Releases, 1);
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
        QVERIFY(!capture(kVK_Tab));
        m_Capture->refresh();
        QCOMPARE(m_Releases, 1);
    }
    void disabledTapReleasesBeforeRecovery()
    {
        QVERIFY(capture(kVK_ANSI_A));
        state().handle(kCGEventTapDisabledByTimeout, nullptr);
        QCOMPARE(m_Releases, 1);
        QVERIFY(state().pending.empty());
        state().active = true;
        QVERIFY(capture(kVK_Space));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(1));
        QCOMPARE(keys[0].code, kVK_Space);
        state().handle(kCGEventTapDisabledByUserInput, nullptr);
        QCOMPARE(m_Releases, 2);
    }
    void overflowFailsOpenAndReleasesRemoteKeys()
    {
        for (unsigned i = 0; i < state().QueueLimit; ++i) QVERIFY(capture(kVK_ANSI_A));
        QVERIFY(!capture(kVK_ANSI_A));
        QVERIFY(!state().active);
        QVERIFY(drain().empty());
        QCOMPARE(m_Releases, 1);
    }

    void capsLockIsABalancedPress()
    {
        QVERIFY(capture(kVK_CapsLock, kCGEventFlagsChanged, kCGEventFlagMaskAlphaShift));
        auto keys = drain();
        QCOMPARE(keys.size(), size_t(2));
        QCOMPARE(keys[0].code, kVK_CapsLock);
        QVERIFY(keys[0].down && !keys[1].down);
    }
    void isoAnsiAndJisMappingsPreserveNativeKeys()
    {
        QVERIFY(capture(10));
        QCOMPARE(drain().back().code, 10u);
        state().isoKeyboard = true;
        QVERIFY(capture(10));
        QCOMPARE(drain().back().code, 50u);
        QVERIFY(capture(50));
        QCOMPARE(drain().back().code, 10u);
        QVERIFY(capture(93));
        QCOMPARE(drain().back().code, 93u);
    }
    void gesturesAndHardwareControlsStayLocal()
    {
        auto event = CGEventCreate(nullptr);
        QVERIFY(state().handle(kCGEventScrollWheel, event) == event);
        QVERIFY(state().handle(kCGEventMouseMoved, event) == event);
        CFRelease(event);
        QVERIFY(!capture(kVK_Function, kCGEventFlagsChanged));
        QVERIFY(!capture(kVK_VolumeUp));
        QVERIFY(drain().empty());
    }
    void teardownReleasesPendingKeys()
    {
        QVERIFY(capture(kVK_ANSI_A));
        QVERIFY(!state().pending.empty());
        m_Capture.reset();
        QCOMPARE(m_Releases, 1);
    }
    void nativeForceQuitRemainsLocal()
    {
        QVERIFY(!capture(kVK_Escape, kCGEventKeyDown,
                         kCGEventFlagMaskCommand | kCGEventFlagMaskAlternate));
        QVERIFY(!state().active);
        QCOMPARE(m_Releases, 1);
        QVERIFY(drain().empty());
    }

};

QTEST_GUILESS_MAIN(TestMacKeyboardCapture)
#include "test_mackeyboardcapture.moc"
