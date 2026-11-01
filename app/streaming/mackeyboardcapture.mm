#include <QDebug>
#include "mackeyboardcapture.h"
#include "qt/mackeymap.h"
#include <iterator>

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <IOKit/hidsystem/ev_keymap.h>
#include <deque>
#include <utility>

namespace {
bool accessibilityPromptNeeded(bool captureEnabled, bool trusted, bool& prompted)
{
    if (!captureEnabled || trusted || prompted) return false;
    prompted = true;
    return true;
}

struct Modifier {
    std::uint32_t code;
    std::uint16_t bit;
    CGEventFlags side;
    CGEventFlags otherSide;
    CGEventFlags either;
};
constexpr Modifier modifiers[] = {
    {kVK_Shift, 1, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, kCGEventFlagMaskShift},
    {kVK_RightShift, 2, NX_DEVICERSHIFTKEYMASK, NX_DEVICELSHIFTKEYMASK, kCGEventFlagMaskShift},
    {kVK_Control, 4, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK, kCGEventFlagMaskControl},
    {kVK_RightControl, 8, NX_DEVICERCTLKEYMASK, NX_DEVICELCTLKEYMASK, kCGEventFlagMaskControl},
    {kVK_Option, 16, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK, kCGEventFlagMaskAlternate},
    {kVK_RightOption, 32, NX_DEVICERALTKEYMASK, NX_DEVICELALTKEYMASK, kCGEventFlagMaskAlternate},
    {kVK_Command, 64, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK, kCGEventFlagMaskCommand},
    {kVK_RightCommand, 128, NX_DEVICERCMDKEYMASK, NX_DEVICELCMDKEYMASK, kCGEventFlagMaskCommand},
};

std::uint16_t modifiersFromFlags(CGEventFlags flags)
{
    std::uint16_t result = 0;
    for (unsigned i = 0; i < std::size(modifiers); ++i) {
        const auto& m = modifiers[i];
        // Synthetic/device-independent flags have no side bits. Pick the left
        // side once, not two presses for the same aggregate modifier.
        bool pressed = (flags & m.either) && ((flags & m.side) ||
            (!(i & 1) && !(flags & (m.side | m.otherSide))));
        if (pressed) result |= m.bit;
    }
    if (flags & kCGEventFlagMaskAlphaShift) result |= 256;
    return result;
}
}

struct MacKeyboardCapture::State
{
    std::function<bool()> ownsKeyboard;
    std::function<void()> releaseKeys;
    std::function<bool()> queueWake;
    std::function<bool()> isTrusted = [] { return AXIsProcessTrusted(); };
    CFMachPortRef tap = nullptr;
    CFRunLoopSourceRef source = nullptr;
    CFRunLoopTimerRef timer = nullptr;
    std::deque<MacKeyEvent> pending;
    std::uint16_t lastModifiers = 0;
    bool active = false;
    bool dispatching = false;
    bool warned = false;
    bool wakeQueued = false;
    bool isoKeyboard = KBGetLayoutType(LMGetKbdType()) == kKeyboardISO;
    static constexpr unsigned QueueLimit = 256;

    void deactivate()
    {
        // Stop forwarding, not the session's native event source. A disabled
        // tap cannot see the first key after a Spaces transition, leaving
        // recovery dependent on queued Qt focus events or the polling timer.
        // handle() passes every background event through unchanged.
        const bool wasActive = std::exchange(active, false);
        pending.clear();
        wakeQueued = false;
        lastModifiers = 0;
        if (wasActive) releaseKeys();
    }

    void removeTap()
    {
        deactivate();
        if (tap) CGEventTapEnable(tap, false);
        if (source) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
            CFRelease(source);
            source = nullptr;
        }
        if (tap) {
            CFMachPortInvalidate(tap);
            CFRelease(tap);
            tap = nullptr;
        }
    }

    void activate()
    {
        releaseKeys();
        lastModifiers = 0;
        active = true;
        qInfo("macOS system shortcut capture enabled");
    }

    bool enqueue(std::uint32_t code, bool down, std::uint16_t mods)
    {
        if (pending.size() == QueueLimit) return false;
        if (!wakeQueued) {
            if (!queueWake || !queueWake()) return false;
            wakeQueued = true;
        }
        Qt::KeyboardModifiers qtMods;
        if (mods & 3) qtMods |= Qt::ShiftModifier;
        if (mods & 12) qtMods |= Qt::ControlModifier;
        if (mods & 48) qtMods |= Qt::AltModifier;
        if (mods & 192) qtMods |= Qt::MetaModifier;
        pending.push_back({code, down, qtMods});
        return true;
    }

    bool queueKey(CGEventType type, CGEventRef event)
    {
        const auto mods = modifiersFromFlags(CGEventGetFlags(event));
        // Reconcile modifiers before every key, including keys held before
        // focus was gained. No parallel Qt/native path forwards the same key.
        for (const auto& m : modifiers) {
            if ((mods & m.bit) != (lastModifiers & m.bit) &&
                !enqueue(m.code, (mods & m.bit) != 0, mods)) return false;
        }
        const bool capsChanged = (mods & 256) != (lastModifiers & 256);
        lastModifiers = mods;
        if (type == kCGEventFlagsChanged) {
            if (CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode) == kVK_CapsLock && capsChanged)
                return enqueue(kVK_CapsLock, true, mods) &&
                       enqueue(kVK_CapsLock, false, mods);
            return true;
        }
        // The Host owns key repeat. Do not fill the local queue with repeats.
        if (type == kCGEventKeyDown && CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat))
            return true;
        const auto raw = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        auto code = raw;
        if (isoKeyboard && (code == 10 || code == 50)) code = 60 - code;
        if (code < 0 || code >= 128 || !qtMacRemoteKey(static_cast<std::uint32_t>(code)).vk) return false;
        return enqueue(static_cast<std::uint32_t>(code), type == kCGEventKeyDown, mods);
    }

    CGEventRef handle(CGEventType type, CGEventRef event)
    {
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            // Drop pending input and release the remote keyboard before the
            // timer rechecks permission/focus and attempts recovery.
            deactivate();
            qWarning("macOS shortcut capture interrupted; remote keys released");
            return event;
        }
        if (!ownsKeyboard()) {
            deactivate();
            return event;
        }
        if (type != kCGEventKeyDown && type != kCGEventKeyUp && type != kCGEventFlagsChanged)
            return event;
        // Fn and media/system-defined events remain local. Never intercept
        // pointing-device or Spaces gesture events.
        const auto raw = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        const auto flags = CGEventGetFlags(event);
        if (raw == kVK_Escape && (flags & kCGEventFlagMaskCommand) &&
            (flags & kCGEventFlagMaskAlternate)) {
            // Keep the local emergency Force Quit chord available even if the
            // OS version happens to expose it to session taps.
            deactivate();
            return event;
        }
        if (raw == kVK_Function || raw == kVK_VolumeUp || raw == kVK_VolumeDown || raw == kVK_Mute)
            return event;
        // Native focus and the user's capture policy are authoritative. Resume
        // on the first returned key, even without a Qt focus notification or
        // a timer tick. Permission revocation must never re-arm forwarding.
        if (!active) {
            if (!isTrusted()) return event;
            activate();
        }
        if (!queueKey(type, event)) {
            deactivate();
            qWarning("Unable to queue macOS shortcut; capture released");
            return event;
        }
        return nullptr; // The existing PLANK input path, never the local OS.
    }

    static CGEventRef callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* context)
    {
        @autoreleasepool { return static_cast<State*>(context)->handle(type, event); }
    }

    void refresh()
    {
        if (!ownsKeyboard()) { deactivate(); return; }
        if (!isTrusted()) {
            // A grant after revocation must create a fresh authorized tap,
            // not try to reuse a port that TCC may have permanently disabled.
            removeTap();
            // Permission UI belongs to the launcher/settings, never a stream
            // whose pointer capture could make the system dialog inaccessible.
            if (!warned) {
                qWarning("System shortcut capture needs PLANK Client Accessibility permission in System Settings > Privacy & Security. Local OS shortcuts remain active until approved.");
                warned = true;
            }
            return;
        }
        if (tap && !CFMachPortIsValid(tap)) {
            removeTap();
        }
        if (active && tap && !CGEventTapIsEnabled(tap)) deactivate();
        if (!tap) {
            const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) |
                CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);
            tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                  kCGEventTapOptionDefault, mask, callback, this);
            if (tap) {
                source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
                if (source) CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
                else { CFMachPortInvalidate(tap); CFRelease(tap); tap = nullptr; }
            }
        }
        if (!tap) {
            if (!warned) qWarning("macOS system shortcut capture is unavailable");
            warned = true;
            return;
        }
        if (!active) {
            CGEventTapEnable(tap, true);
            if (!CGEventTapIsEnabled(tap)) {
                removeTap();
                if (!warned) qWarning("macOS system shortcut capture could not be enabled");
                warned = true;
                return;
            }
            activate();
        }
        warned = false;
    }
};

void MacKeyboardCapture::requestPermissionIfNeeded(bool captureEnabled)
{
    NSCAssert(NSThread.isMainThread, @"Keyboard permission UI is main-thread only");
    static bool prompted = false;
    if (accessibilityPromptNeeded(captureEnabled, AXIsProcessTrusted(), prompted)) {
        AXIsProcessTrustedWithOptions((CFDictionaryRef)@{(id)kAXTrustedCheckOptionPrompt: @YES});
    }
}

MacKeyboardCapture::MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                                       std::function<void()> releaseKeys)
    : MacKeyboardCapture(std::move(ownsKeyboard), std::move(releaseKeys), true) {}

MacKeyboardCapture::MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                                       std::function<void()> releaseKeys, bool monitor)
    : m_State(std::make_unique<State>())
{
    m_State->ownsKeyboard = std::move(ownsKeyboard);
    m_State->releaseKeys = std::move(releaseKeys);
    if (monitor) {
        CFRunLoopTimerContext context {0, m_State.get(), nullptr, nullptr, nullptr};
        m_State->timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 1.0, 0, 0,
            [](CFRunLoopTimerRef, void* data) { @autoreleasepool { static_cast<State*>(data)->refresh(); } },
            &context);
        if (m_State->timer)
            CFRunLoopAddTimer(CFRunLoopGetMain(), m_State->timer, kCFRunLoopCommonModes);
    }
}

MacKeyboardCapture::~MacKeyboardCapture()
{
    NSCAssert(NSThread.isMainThread, @"Keyboard capture is main-thread only");
    if (m_State->timer) { CFRunLoopTimerInvalidate(m_State->timer); CFRelease(m_State->timer); }
    m_State->removeTap();
}

void MacKeyboardCapture::refresh()
{
    NSCAssert(NSThread.isMainThread, @"Keyboard capture is main-thread only");
    m_State->refresh();
}

void MacKeyboardCapture::setQueueWake(std::function<bool()> wake)
{
    NSCAssert(NSThread.isMainThread, @"Keyboard capture is main-thread only");
    NSCAssert(!m_State->active, @"Choose the dispatcher before enabling capture");
    m_State->queueWake = std::move(wake);
}

void MacKeyboardCapture::dispatchPending(const std::function<void(const MacKeyEvent&)>& deliver)
{
    NSCAssert(NSThread.isMainThread, @"Keyboard capture is main-thread only");
    m_State->wakeQueued = false;
    while (!m_State->pending.empty()) {
        if (!m_State->active || !m_State->ownsKeyboard()) { m_State->deactivate(); break; }
        auto key = m_State->pending.front();
        m_State->pending.pop_front();
        m_State->dispatching = true;
        deliver(key);
        m_State->dispatching = false;
    }
}

bool MacKeyboardCapture::capturesKeys() const
{
    // The tap has already queued this input. Also exclude native key events
    // still in the GUI queue when capture began. Explicit menu/Dock Quit is not a key.
    return m_State->active && !m_State->dispatching;
}
