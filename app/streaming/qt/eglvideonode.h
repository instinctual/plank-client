#pragma once

#include "sessiongeometry.h"
#include "vaapivideoframe.h"

#include <QQuickItem>
#include <atomic>

struct QtEglVideoStatus
{
    enum Error { None, WrongBackend, ImportFailed, PrecisionOrDrawFailed, FenceFailed };
    std::atomic<Error> error{None};
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> imports{0};
};

// A root-sized underlay, behind QML controls. Qt owns its native surface and
// render thread. Call setFrame/setPresentation on the GUI thread only; the
// future session controller must coalesce decoder deliveries before this edge.
class QtEglVideoItem final : public QQuickItem
{
public:
    explicit QtEglVideoItem(QQuickItem* parent = nullptr);
    void setFrame(QtVaapiVideoFrame::Ptr frame);
    void setPresentation(const QtSessionGeometry& geometry);
    std::shared_ptr<const QtEglVideoStatus> status() const { return m_Status; }

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;

private:
    QtVaapiVideoFrame::Ptr m_Frame;
    QtSessionGeometry m_Geometry;
    std::shared_ptr<QtEglVideoStatus> m_Status = std::make_shared<QtEglVideoStatus>();
};
