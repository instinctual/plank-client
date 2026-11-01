#import <AppKit/AppKit.h>
#include <QtTest>
#include <QTimer>
#include "../../app/streaming/macquitshortcut.h"

// Harmless target: exercise the real native Quit selector without terminating
// the test process. No remote session or system input injection is involved.
@interface QuitTarget : NSObject
@property(nonatomic) NSInteger calls;
- (void)terminate:(id)sender;
@end
@implementation QuitTarget
- (void)terminate:(id)sender { Q_UNUSED(sender); self.calls++; }
@end

class TestMacQuitShortcut : public QObject
{
    Q_OBJECT
    NSMenu* m_PreviousMenu = nil;
    NSMenu* m_Menu = nil;
    NSMenu* m_AppMenu = nil;
    NSMenuItem* m_Quit = nil;
    QuitTarget* m_Target = nil;
    bool m_Captured = false;

    NSEvent* commandQ()
    {
        return [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint
            modifierFlags:NSEventModifierFlagCommand timestamp:0 windowNumber:0
            context:nil characters:@"q" charactersIgnoringModifiers:@"q"
            isARepeat:NO keyCode:12];
    }

private slots:
    void initTestCase() { [NSApplication sharedApplication]; }
    void init()
    {
        m_PreviousMenu = [NSApp.mainMenu retain];
        m_Menu = [[NSMenu alloc] initWithTitle:@"Test"];
        m_AppMenu = [[NSMenu alloc] initWithTitle:@"Application"];
        m_AppMenu.autoenablesItems = NO;
        NSMenuItem* root = [m_Menu addItemWithTitle:@"Application" action:nil keyEquivalent:@""];
        root.submenu = m_AppMenu;
        m_Target = [[QuitTarget alloc] init];
        m_Quit = [m_AppMenu addItemWithTitle:@"Localized title" action:@selector(terminate:)
            keyEquivalent:@"q"];
        m_Quit.target = m_Target;
        m_Quit.keyEquivalentModifierMask = NSEventModifierFlagCommand;
        NSApp.mainMenu = m_Menu;
        m_Captured = false;
    }
    void cleanup()
    {
        NSApp.mainMenu = m_PreviousMenu;
        [m_PreviousMenu release];
        [m_AppMenu release];
        [m_Menu release];
        [m_Target release];
    }
    void uncapturedShortcutQuitsLocally()
    {
        MacQuitShortcut guard([this] { return m_Captured; });
        QVERIFY([m_Menu performKeyEquivalent:commandQ()]);
        QCOMPARE(m_Target.calls, 1);
    }
    void capturedShortcutDoesNotQuit()
    {
        m_Captured = true;
        MacQuitShortcut guard([this] { return m_Captured; });
        QVERIFY(![m_Menu performKeyEquivalent:commandQ()]);
        QCOMPARE(m_Target.calls, 0);
        QVERIFY(m_Quit.enabled);
        QVERIFY(m_Quit.action == @selector(terminate:));
    }
    void explicitQuitStillWorksWhileCaptured()
    {
        m_Captured = true;
        MacQuitShortcut guard([this] { return m_Captured; });
        [m_AppMenu performActionForItemAtIndex:0];
        QCOMPARE(m_Target.calls, 1);
    }
    void focusAndCaptureChangesRestoreShortcut()
    {
        MacQuitShortcut guard([this] { return m_Captured; });
        for (int i = 0; i < 3; ++i) {
            m_Captured = true;
            guard.refresh();
            QVERIFY(![m_Menu performKeyEquivalent:commandQ()]);
            m_Captured = false;
            guard.refresh();
            QVERIFY([m_Menu performKeyEquivalent:commandQ()]);
        }
        QCOMPARE(m_Target.calls, 3);
    }
    void teardownRestoresOriginalShortcut()
    {
        m_Quit.keyEquivalent = @"x";
        m_Quit.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
        {
            m_Captured = true;
            MacQuitShortcut guard([this] { return m_Captured; });
            QVERIFY([m_Quit.keyEquivalent isEqualToString:@""]);
        }
        QVERIFY([m_Quit.keyEquivalent isEqualToString:@"x"]);
        QCOMPARE(m_Quit.keyEquivalentModifierMask,
                 NSEventModifierFlagCommand | NSEventModifierFlagOption);
    }
    void nativeDispatchRefreshesAfterMenuResync()
    {
        MacQuitShortcut guard([this] { return m_Captured; });
        // No explicit refresh: verify the monitor runs before native matching.
        m_Captured = true;
        [NSApp sendEvent:commandQ()];
        QVERIFY([m_Quit.keyEquivalent isEqualToString:@""]);
        QCOMPARE(m_Target.calls, 0);
        m_Quit.keyEquivalent = @"q"; // Simulate Qt menu synchronization.
        [NSApp sendEvent:commandQ()];
        QVERIFY([m_Quit.keyEquivalent isEqualToString:@""]);
        QCOMPARE(m_Target.calls, 0);
        m_Captured = false;
        [NSApp sendEvent:commandQ()];
        QVERIFY([m_Quit.keyEquivalent isEqualToString:@"q"]);
    }
    void keepsAlreadyQueuedRemoteKey()
    {
        int delivered = 0;
        m_Captured = true;
        MacQuitShortcut guard([this] { return m_Captured; });
        QTimer::singleShot(0, this, [&] { ++delivered; });
        [NSApp sendEvent:commandQ()];
        QCOMPARE(m_Target.calls, 0);
        QTRY_COMPARE(delivered, 1);
    }
};

QTEST_GUILESS_MAIN(TestMacQuitShortcut)
#include "test_macquitshortcut.moc"
