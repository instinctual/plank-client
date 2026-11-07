#pragma once

#include <cstdint>
#include <functional>

// Aggregate observations only. Coordinates are transient comparison state and
// never leave the input reader or enter the metrics callback.
struct PlankPenActivity
{
    bool observed = false;
    std::uint64_t motion = 0;
    std::uint64_t tipDown = 0;
};

using PlankPenObserver = std::function<void(const PlankPenActivity&)>;

class PlankPenActivityState
{
public:
    void reset() { *this = {}; }
    void prime(std::int64_t x, std::int64_t y, bool near, bool down)
    {
        m_X = x; m_Y = y; m_Near = near; m_Down = near && down; m_Known = true;
    }
    PlankPenActivity sample(std::int64_t x, std::int64_t y, bool near, bool down)
    {
        PlankPenActivity result;
        result.observed = true;
        result.motion = m_Known && m_Near && near && (x != m_X || y != m_Y);
        result.tipDown = m_Known && near && down && !m_Down;
        prime(x, y, near, down);
        return result;
    }

private:
    std::int64_t m_X = 0, m_Y = 0;
    bool m_Near = false, m_Down = false, m_Known = false;
};
