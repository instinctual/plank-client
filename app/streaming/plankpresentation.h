#pragma once

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QVector>

#include <SDL3/SDL.h>

struct PlankPresentationOutput
{
    SDL_Window* window = nullptr;
    QRect canvasRect;
    bool primary = false;
};

struct PlankPresentationLayout
{
    QSize canvasSize;
    QVector<PlankPresentationOutput> outputs;

    bool isMultiOutput() const
    {
        return outputs.size() > 1;
    }
};

struct PlankPresentationSlice
{
    QRectF sourceRect;
    QRect destinationRect;
    bool visible = false;
};

class PlankPresentation
{
public:
    // Construct a shared canvas from Host output sizes in desktop order.
    static QVector<QRect> horizontalCanvas(const QVector<QSize>& outputSizes);

    // Leave the native fullscreen Space before hiding a secondary surface.
    static bool setSecondaryFullscreen(SDL_Window* window, bool fullscreen);

    static QRect videoRect(const QSize& streamSize, const QSize& canvasSize);

    static PlankPresentationSlice sliceForOutput(
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect);

    // Convert the shared canvas slice to an individual window's backing pixels.
    // Input uses logical window coordinates against the same canvas rectangle.
    static PlankPresentationSlice sliceForDrawable(
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        const QSize& drawableSize);

    // Captured drag events stay relative to the window where the press began.
    // Resolve them in desktop logical coordinates before applying per-output DPI.
    // Outside every presentation window, retain the source for normal clamping.
    static int resolvePointerOutput(const QVector<QRect>& windowRects,
                                    int sourceOutput, const QPointF& sourcePoint,
                                    QPointF& outputPoint);

    static bool mapWindowPointToStream(
        const QPointF& windowPoint,
        const QSize& windowSize,
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        QPointF& streamPoint,
        bool allowClampedPosition);

    static bool mapStreamPointToWindow(
        const QPointF& streamPoint,
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        const QSize& windowSize,
        QPointF& windowPoint);
};
