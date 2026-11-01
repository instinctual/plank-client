#pragma once

namespace MacDisplayGeometry {
// Each presentation window uses native fullscreen, including mixed-DPI
// multi-display sessions. Match Client must use the same camera-safe viewport.
inline bool useNativeFullscreen(int connectedDisplayCount)
{
    return connectedDisplayCount > 0;
}

// AppKit 15 reserves five additional logical points below the camera when
// placing a native fullscreen window. NSScreen.safeAreaInsets omits this
// margin (38-point inset, but 43-point fullscreen exclusion on a scaled panel).
// Keep this measured compatibility correction scoped to macOS 15; do not
// change newer systems without measuring their settled native window bounds.
inline int nativeFullscreenTopInset(int cameraInset, int osMajor)
{
    return cameraInset > 0 && osMajor == 15 ? cameraInset + 5 : cameraInset;
}

// Match the native fullscreen viewport without changing Retina density.
// AppKit supplies the inset; zero means the complete panel remains usable.
inline bool insetTop(int logicalWidth, int& logicalHeight,
                     int pixelWidth, int& pixelHeight, int top)
{
    if (logicalWidth <= 0 || logicalHeight <= 0 || top < 0 || top >= logicalHeight)
        return false;
    const int scale = pixelWidth / logicalWidth;
    if ((scale != 1 && scale != 2) || pixelWidth != logicalWidth * scale ||
            pixelHeight != logicalHeight * scale)
        return false;
    logicalHeight -= top;
    pixelHeight = logicalHeight * scale;
    return true;
}
}
