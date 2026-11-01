#pragma once

#include <QRect>
#include <QSize>
#include <cstdint>

// Read-only CoreGraphics/AppKit display snapshots. Qt owns the session windows.
namespace MacDisplays {
bool currentMode(std::uint32_t display, QSize& pixels, QRect& bounds, bool fullscreen);
bool currentModeForBounds(const QRect& bounds, QSize& pixels, QRect& matchedBounds, bool fullscreen);
}
