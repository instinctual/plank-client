#include "cursoritem.h"
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

QSGNode* QtTabletCursorItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    struct Node final : QSGSimpleTextureNode { quint64 serial = 0; };
    auto* node = static_cast<Node*>(old);
    if (m_Image.isNull()) { delete node; return nullptr; }
    if (!node) { node = new Node; node->setOwnsTexture(true); }
    if (node->serial != m_Serial) {
        auto* texture = window()->createTextureFromImage(m_Image);
        if (!texture) { delete node; return nullptr; }
        // The node owns its texture. setTexture() deletes the previous one;
        // deleting it here as well crashes when a tablet cursor shape changes.
        node->setTexture(texture);
        node->serial = m_Serial;
        node->setFiltering(QSGTexture::Linear);
    }
    node->setRect(boundingRect());
    return node;
}
