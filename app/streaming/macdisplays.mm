#include "macdisplays.h"
#include "macdisplaymode.h"
#include "macdisplaygeometry.h"
#import <AppKit/AppKit.h>
#include <QDebug>
#include <cmath>

namespace MacDisplays {
bool currentMode(std::uint32_t display, QSize& pixels, QRect& bounds, bool fullscreen)
{
    @autoreleasepool {
        MacDisplayMode::Snapshot current;
        if (!MacDisplayMode::currentSnapshot(display, current)) return false;
        const CGRect logical = CGDisplayBounds(display);
        bounds = {qRound(logical.origin.x), qRound(logical.origin.y),
                  qRound(logical.size.width), qRound(logical.size.height)};
        int height = bounds.height();
        if (fullscreen) {
            NSScreen* selected = nil;
            for (NSScreen* screen in NSScreen.screens) {
                if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == display) {
                    selected = screen; break;
                }
            }
            if (!selected) return false;
            const int top = MacDisplayGeometry::nativeFullscreenTopInset(
                static_cast<int>(std::ceil(selected.safeAreaInsets.top)),
                static_cast<int>(NSProcessInfo.processInfo.operatingSystemVersion.majorVersion));
            if (!MacDisplayGeometry::insetTop(bounds.width(), height, current.width, current.height, top)) return false;
            bounds.translate(0, top);
            bounds.setHeight(height);
            qInfo("PLANK Mac display: fullscreen viewport=%dx%d backing=%dx%d top-inset=%d",
                  bounds.width(), height, current.width, current.height, top);
        }
        pixels = {current.width, current.height};
        return pixels.isValid() && bounds.isValid();
    }
}

bool currentModeForBounds(const QRect& bounds, QSize& pixels, QRect& matchedBounds, bool fullscreen)
{
    CGDirectDisplayID ids[16], selected = 0;
    std::uint32_t count = 0;
    if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess) return false;
    for (std::uint32_t i = 0; i < count; ++i) {
        const CGRect cg = CGDisplayBounds(ids[i]);
        if (QRect(qRound(cg.origin.x), qRound(cg.origin.y), qRound(cg.size.width), qRound(cg.size.height)) == bounds) {
            if (selected) return false;
            selected = ids[i];
        }
    }
    return selected && currentMode(selected, pixels, matchedBounds, fullscreen);
}
}
