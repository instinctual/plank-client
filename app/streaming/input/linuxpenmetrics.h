#pragma once

#include "penmetrics.h"
#include <linux/input.h>

// Parse the kernel's already-decoded pen events. Raw HID forwarding does not
// depend on this observer. Queue overflow requires a fresh kernel snapshot;
// no motion or tip transition is invented during resynchronization.
class LinuxPenActivityTracker
{
public:
    bool ready() const { return m_Ready; }
    void reset() { *this = {}; }
    void prime(int x, int y, bool pen, bool eraser, bool down)
    {
        m_X = x; m_Y = y; m_Pen = pen; m_Eraser = eraser; m_Down = down;
        m_State.prime(x, y, pen || eraser, down);
        m_Ready = true;
    }
    PlankPenActivity consume(const input_event& event)
    {
        if (event.type == EV_SYN && event.code == SYN_DROPPED) { reset(); return {}; }
        if (!m_Ready) return {};
        if (event.type == EV_ABS) {
            if (event.code == ABS_X) m_X = event.value;
            else if (event.code == ABS_Y) m_Y = event.value;
        } else if (event.type == EV_KEY) {
            if (event.code == BTN_TOOL_PEN) m_Pen = event.value != 0;
            else if (event.code == BTN_TOOL_RUBBER) m_Eraser = event.value != 0;
            else if (event.code == BTN_TOUCH) m_Down = event.value != 0;
        } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
            return m_State.sample(m_X, m_Y, m_Pen || m_Eraser, m_Down);
        }
        return {};
    }

private:
    PlankPenActivityState m_State;
    int m_X = 0, m_Y = 0;
    bool m_Pen = false, m_Eraser = false, m_Down = false, m_Ready = false;
};
