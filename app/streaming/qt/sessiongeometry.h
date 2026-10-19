#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

// The presenter supplies this slice. UI code must not calculate an independent
// video aspect ratio, use the Host dimensions as a window size, or read a
// desktop-global pointer. All rectangles are half-open pixel extents.
struct QtSessionGeometry
{
    QSize streamSize;
    QRectF sourcePixels;
    QRectF destinationPixels;
    QSize drawableSize;
    QSize logicalSize;

    bool valid() const
    {
        auto finiteRect = [](const QRectF& r) {
            return std::isfinite(r.x()) && std::isfinite(r.y()) &&
                   std::isfinite(r.width()) && std::isfinite(r.height());
        };
        return streamSize.width() > 0 && streamSize.height() > 0 &&
               drawableSize.width() > 0 && drawableSize.height() > 0 &&
               logicalSize.width() > 0 && logicalSize.height() > 0 &&
               finiteRect(sourcePixels) && finiteRect(destinationPixels) &&
               sourcePixels.width() >= 1 && sourcePixels.height() >= 1 &&
               destinationPixels.width() > 0 && destinationPixels.height() > 0 &&
               QRectF(QPointF(), streamSize).contains(sourcePixels) &&
               QRectF(QPointF(), drawableSize).contains(destinationPixels);
    }

    bool toStream(const QPointF& logical, QPointF& stream, bool clamp = false) const
    {
        if (!valid() || !std::isfinite(logical.x()) || !std::isfinite(logical.y())) {
            return false;
        }
        const QPointF pixel(logical.x() * drawableSize.width() / logicalSize.width(),
                            logical.y() * drawableSize.height() / logicalSize.height());
        const auto& d = destinationPixels;
        if (!clamp && (pixel.x() < d.x() || pixel.x() >= d.x() + d.width() ||
                       pixel.y() < d.y() || pixel.y() >= d.y() + d.height())) {
            return false;
        }
        stream = QPointF(
            sourcePixels.x() + std::clamp((pixel.x() - d.x()) / d.width() *
                                         sourcePixels.width(), 0.0, sourcePixels.width() - 1.0),
            sourcePixels.y() + std::clamp((pixel.y() - d.y()) / d.height() *
                                         sourcePixels.height(), 0.0, sourcePixels.height() - 1.0));
        return true;
    }

    bool fromStream(const QPointF& stream, QPointF& logical) const
    {
        if (!valid() || !std::isfinite(stream.x()) || !std::isfinite(stream.y()) ||
                stream.x() < sourcePixels.x() || stream.x() >= sourcePixels.x() + sourcePixels.width() ||
                stream.y() < sourcePixels.y() || stream.y() >= sourcePixels.y() + sourcePixels.height()) {
            return false;
        }
        logical = QPointF(
            (destinationPixels.x() + (stream.x() - sourcePixels.x()) *
             destinationPixels.width() / sourcePixels.width()) * logicalSize.width() / drawableSize.width(),
            (destinationPixels.y() + (stream.y() - sourcePixels.y()) *
             destinationPixels.height() / sourcePixels.height()) * logicalSize.height() / drawableSize.height());
        return true;
    }
};

struct QtSessionOutputGeometry
{
    QRectF desktopLogical;
    QtSessionGeometry presentation;
};

// Route a grabbed mouse event through the same per-output slices used to draw
// video. The position comes from the event's local coordinates plus its known
// output origin, not a second polling/warping cursor coordinate system.
inline bool qtMapDraggedPointer(const QVector<QtSessionOutputGeometry>& outputs,
                                QPointF desktopLogical, QPointF& stream)
{
    if (!std::isfinite(desktopLogical.x()) || !std::isfinite(desktopLogical.y())) return false;
    const QtSessionOutputGeometry* nearest = nullptr;
    qreal distance = std::numeric_limits<qreal>::max();
    for (const auto& output : outputs) {
        const auto& rect = output.desktopLogical;
        if (!rect.isValid() || !output.presentation.valid()) continue;
        const auto x = std::clamp(desktopLogical.x(), rect.left(), rect.right());
        const auto y = std::clamp(desktopLogical.y(), rect.top(), rect.bottom());
        const qreal dx = desktopLogical.x() - x, dy = desktopLogical.y() - y;
        const qreal candidate = dx * dx + dy * dy;
        if (desktopLogical.x() >= rect.left() && desktopLogical.x() < rect.right() &&
                desktopLogical.y() >= rect.top() && desktopLogical.y() < rect.bottom()) {
            nearest = &output;
            break;
        }
        if (candidate < distance) { distance = candidate; nearest = &output; }
    }
    if (!nearest) return false;
    return nearest->presentation.toStream(desktopLogical - nearest->desktopLogical.topLeft(), stream, true);
}
