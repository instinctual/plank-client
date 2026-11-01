#include "macquitshortcut.h"

#import <AppKit/AppKit.h>
#include <utility>

namespace {
NSMenuItem* findQuitItem(NSMenu* menu)
{
    for (NSMenuItem* item in menu.itemArray) {
        // Qt's default application menu uses terminate:. Do not
        // depend on the localized title, menu position or the physical Q key.
        if (item.action == @selector(terminate:))
            return item;
        if (NSMenuItem* found = findQuitItem(item.submenu))
            return found;
    }
    return nil;
}
}

struct MacQuitShortcut::State
{
    std::function<bool()> remoteOwnsKeyboard;
    id monitor = nil;
    NSMenuItem* item = nil;
    NSString* originalKey = nil;
    NSEventModifierFlags originalModifiers = 0;

    void restore()
    {
        if (item) {
            item.keyEquivalent = originalKey;
            item.keyEquivalentModifierMask = originalModifiers;
            [item release];
            [originalKey release];
            item = nil;
            originalKey = nil;
        }
    }
};

MacQuitShortcut::MacQuitShortcut(std::function<bool()> remoteOwnsKeyboard)
    : m_State(std::make_unique<State>())
{
    m_State->remoteOwnsKeyboard = std::move(remoteOwnsKeyboard);
    // Refresh before AppKit matches menu shortcuts: focus/fullscreen changes
    // or Qt native-menu synchronization may have happened since the last refresh.
    // Return the ORIGINAL event; never synthesize a second remote key event.
    m_State->monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
        handler:^NSEvent* (NSEvent* event) {
            refresh();
            return event;
        }];
    refresh();
}

MacQuitShortcut::~MacQuitShortcut()
{
    if (m_State->monitor)
        [NSEvent removeMonitor:m_State->monitor];
    m_State->restore();
}

void MacQuitShortcut::refresh()
{
    NSCAssert([NSThread isMainThread], @"Quit shortcut ownership is main-thread only");
    if (!m_State->remoteOwnsKeyboard()) {
        m_State->restore();
        return;
    }

    NSMenuItem* item = findQuitItem(NSApp.mainMenu);
    if (item != m_State->item) {
        m_State->restore();
        if (!item)
            return;
        m_State->item = [item retain];
        m_State->originalKey = [item.keyEquivalent copy];
        m_State->originalModifiers = item.keyEquivalentModifierMask;
    }
    // Remove ONLY the shortcut, not the action/enabled state. Clicking Quit
    // and Dock/Apple-event termination still follow normal application exit.
    item.keyEquivalent = @"";
}
