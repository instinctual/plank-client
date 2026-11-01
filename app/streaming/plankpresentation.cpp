#include "plankpresentation.h"

#include <QtMath>

QRect PlankPresentation::videoRect(const QSize& streamSize,
                                             const QSize& canvasSize)
{
    if (!streamSize.isValid() || !canvasSize.isValid()) {
        return QRect();
    }

    const qreal widthScale = static_cast<qreal>(canvasSize.width()) /
            streamSize.width();
    const qreal heightScale = static_cast<qreal>(canvasSize.height()) /
            streamSize.height();
    const qreal scale = qMin(widthScale, heightScale);
    const int width = qMax(1, qRound(streamSize.width() * scale));
    const int height = qMax(1, qRound(streamSize.height() * scale));
    return QRect((canvasSize.width() - width) / 2,
                 (canvasSize.height() - height) / 2,
                 width, height);
}

PlankPresentationSlice PlankPresentation::sliceForOutput(
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect)
{
    PlankPresentationSlice slice;
    const QRect destination = videoRect(streamSize, canvasSize);
    const QRect visible = destination.intersected(outputCanvasRect);
    if (visible.isEmpty()) {
        return slice;
    }

    const qreal sourceScaleX = static_cast<qreal>(streamSize.width()) /
            destination.width();
    const qreal sourceScaleY = static_cast<qreal>(streamSize.height()) /
            destination.height();
    slice.sourceRect = QRectF(
        (visible.left() - destination.left()) * sourceScaleX,
        (visible.top() - destination.top()) * sourceScaleY,
        visible.width() * sourceScaleX,
        visible.height() * sourceScaleY);
    slice.destinationRect = visible.translated(-outputCanvasRect.topLeft());
    slice.visible = true;
    return slice;
}

PlankPresentationSlice PlankPresentation::sliceForDrawable(
        const QSize& streamSize, const QSize& canvasSize,
        const QRect& outputCanvasRect, const QSize& drawableSize)
{
    if (!outputCanvasRect.isValid() || !drawableSize.isValid()) {
        return {};
    }
    auto slice = sliceForOutput(streamSize, canvasSize, outputCanvasRect);
    if (slice.visible) {
        const auto& rect = slice.destinationRect;
        const qreal sx = qreal(drawableSize.width()) / outputCanvasRect.width();
        const qreal sy = qreal(drawableSize.height()) / outputCanvasRect.height();
        const int left = qRound(rect.x() * sx);
        const int top = qRound(rect.y() * sy);
        slice.destinationRect = QRect(left, top,
            qRound((rect.x() + rect.width()) * sx) - left,
            qRound((rect.y() + rect.height()) * sy) - top);
        slice.visible = !slice.destinationRect.isEmpty();
    }
    return slice;
}

int PlankPresentation::resolvePointerOutput(const QVector<QRect>& windowRects,
                                           int sourceOutput,
                                           const QPointF& sourcePoint,
                                           QPointF& outputPoint)
{
    outputPoint = sourcePoint;
    if (sourceOutput < 0 || sourceOutput >= windowRects.size() ||
            !windowRects.at(sourceOutput).isValid()) {
        return -1;
    }
    const QPointF desktopPoint = sourcePoint + windowRects.at(sourceOutput).topLeft();
    for (int i = 0; i < windowRects.size(); ++i) {
        const auto& rect = windowRects.at(i);
        if (rect.isValid() && desktopPoint.x() >= rect.x() &&
                desktopPoint.x() < rect.x() + rect.width() &&
                desktopPoint.y() >= rect.y() &&
                desktopPoint.y() < rect.y() + rect.height()) {
            outputPoint = desktopPoint - rect.topLeft();
            return i;
        }
    }
    return sourceOutput;
}

bool PlankPresentation::mapWindowPointToStream(
        const QPointF& windowPoint,
        const QSize& windowSize,
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        QPointF& streamPoint,
        bool allowClampedPosition)
{
    if (!windowSize.isValid() || !streamSize.isValid() ||
            !canvasSize.isValid() || !outputCanvasRect.isValid()) {
        return false;
    }

    const QPointF canvasPoint(
        outputCanvasRect.left() +
            windowPoint.x() * outputCanvasRect.width() / windowSize.width(),
        outputCanvasRect.top() +
            windowPoint.y() * outputCanvasRect.height() / windowSize.height());
    const QRect destination = videoRect(streamSize, canvasSize);
    const bool inside = destination.contains(qFloor(canvasPoint.x()),
                                             qFloor(canvasPoint.y()));
    if (!inside && !allowClampedPosition) {
        return false;
    }

    const qreal x = qBound<qreal>(0.0,
        canvasPoint.x() - destination.left(), destination.width());
    const qreal y = qBound<qreal>(0.0,
        canvasPoint.y() - destination.top(), destination.height());
    streamPoint = QPointF(x * streamSize.width() / destination.width(),
                          y * streamSize.height() / destination.height());
    return true;
}

bool PlankPresentation::mapStreamPointToWindow(
        const QPointF& streamPoint,
        const QSize& streamSize,
        const QSize& canvasSize,
        const QRect& outputCanvasRect,
        const QSize& windowSize,
        QPointF& windowPoint)
{
    if (!windowSize.isValid() || !streamSize.isValid() ||
            !canvasSize.isValid() || !outputCanvasRect.isValid()) {
        return false;
    }

    const QRect destination = videoRect(streamSize, canvasSize);
    const QPointF canvasPoint(
        destination.left() + streamPoint.x() * destination.width() /
            streamSize.width(),
        destination.top() + streamPoint.y() * destination.height() /
            streamSize.height());
    if (!outputCanvasRect.contains(qFloor(canvasPoint.x()),
                                   qFloor(canvasPoint.y()))) {
        return false;
    }

    windowPoint = QPointF(
        (canvasPoint.x() - outputCanvasRect.left()) * windowSize.width() /
            outputCanvasRect.width(),
        (canvasPoint.y() - outputCanvasRect.top()) * windowSize.height() /
            outputCanvasRect.height());
    return true;
}
