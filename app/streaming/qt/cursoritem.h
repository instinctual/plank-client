#pragma once
#include <QImage>
#include <QQuickItem>

// Small cursor texture only; decoded video never uses QImage or CPU readback.
// Its position comes exclusively from the Host, mapped through the presenter's
// source/destination rectangles by QtSessionWindow.
class QtTabletCursorItem final : public QQuickItem
{
public:
    explicit QtTabletCursorItem(QQuickItem* parent) : QQuickItem(parent) { setFlag(ItemHasContents); setZ(1); }
    void setImage(QImage image) { m_Image = std::move(image); ++m_Serial; update(); }
protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
private:
    QImage m_Image;
    quint64 m_Serial = 0;
};
