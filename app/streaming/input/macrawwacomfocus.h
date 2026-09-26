#pragma once

#include <optional>

// Main-thread desired focus, separate from the worker's physical device lease.
// The worker still owns attach retries and the acknowledged release barrier.
class MacRawWacomFocus
{
public:
    template<class Outputs, class Window, class HasFocus>
    static bool streamHasFocus(const Outputs& outputs, Window fallback, HasFocus hasFocus)
    {
        if (outputs.begin() == outputs.end())
            return fallback && hasFocus(fallback);
        for (const auto& output : outputs) {
            if (output.window && hasFocus(output.window)) return true;
        }
        return false;
    }

    // A value means the backend needs one transition; nullopt means no change.
    std::optional<bool> update(bool captureActive, bool nativeFocus)
    {
        const bool active = captureActive && nativeFocus;
        if (active == m_Active) return std::nullopt;
        m_Active = active;
        return active;
    }

private:
    bool m_Active = false;
};
