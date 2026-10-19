#pragma once
#include <QObject>
#include <QPointer>
#include <memory>

class QWindow;

// Optional compositor requests on Qt's connection and GUI dispatch queue.
// Never reads, dispatches, flushes, creates or destroys Qt's display/surface.
// Every child proxy is retired before QPlatformSurfaceEvent destroys its parent.
class QtWaylandSessionGuard final : public QObject
{
    Q_OBJECT
public:
    explicit QtWaylandSessionGuard(QObject* parent = nullptr);
    ~QtWaylandSessionGuard() override;
    void setWindows(QWindow* keyboard, QWindow* pointer);
    void setPolicy(bool captureKeys, bool confinePointer);
protected:
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    void refresh();
    void release();
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    QPointer<QWindow> m_KeyboardWindow, m_PointerWindow;
    bool m_CaptureKeys = false, m_Confine = false;
};
