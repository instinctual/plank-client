#pragma once

#include <Qt>
#include <cstdint>
#include <functional>
#include <memory>

struct MacKeyEvent {
    std::uint32_t code = 0;
    bool down = false;
    Qt::KeyboardModifiers modifiers;
};

// Main-thread, session-scoped keyboard capture. The tap only queues events;
// normal session dispatch performs PLANK hotkeys and remote input delivery.
class MacKeyboardCapture
{
public:
    // Call from the launcher/settings UI, never while a stream owns input.
    static void requestPermissionIfNeeded(bool captureEnabled);

    MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                       std::function<void()> releaseKeys);
    ~MacKeyboardCapture();
    void refresh();
    bool capturesKeys() const;
    // Wake Qt to drain the bounded native event-tap queue.
    void setQueueWake(std::function<bool()> wake);
    void dispatchPending(const std::function<void(const MacKeyEvent&)>& deliver);

    MacKeyboardCapture(const MacKeyboardCapture&) = delete;
    MacKeyboardCapture& operator=(const MacKeyboardCapture&) = delete;

private:
    friend class TestMacKeyboardCapture;
    MacKeyboardCapture(std::function<bool()> ownsKeyboard,
                       std::function<void()> releaseKeys, bool monitor);
    struct State;
    std::unique_ptr<State> m_State;
};
