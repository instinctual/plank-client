#pragma once

#include "sessiongeometry.h"

#include <QCursor>
#include <QMap>
#include <QPointer>
#include <QQuickView>
#include <QTimer>

// Ubuntu prototype. Not enabled by shipping app.pro. No SDL window, native
// Wayland proxy, network call or decoder is owned by this GUI-thread component.
class QtSessionWindow : public QQuickView
{
    Q_OBJECT
    Q_PROPERTY(bool toolbarVisible READ toolbarVisible NOTIFY toolbarChanged)
    Q_PROPERTY(bool pinned READ pinned WRITE setPinned NOTIFY toolbarChanged)
    Q_PROPERTY(qreal toolbarPosition READ toolbarPosition NOTIFY toolbarChanged)
    Q_PROPERTY(bool waiting READ waiting WRITE setWaiting NOTIFY waitingChanged)
    Q_PROPERTY(int bitrateKbps READ bitrateKbps WRITE setBitrateKbps NOTIFY bitrateChanged)
    Q_PROPERTY(int minimumBitrateKbps READ minimumBitrateKbps CONSTANT)
    Q_PROPERTY(int maximumBitrateKbps READ maximumBitrateKbps CONSTANT)
    Q_PROPERTY(int bitrateStepKbps READ bitrateStepKbps CONSTANT)
    Q_PROPERTY(QRectF videoRect READ videoRect NOTIFY presentationChanged)
public:
    explicit QtSessionWindow(QWindow* parent = nullptr);

    bool toolbarVisible() const { return m_ToolbarVisible; }
    bool pinned() const { return m_Pinned; }
    qreal toolbarPosition() const { return m_ToolbarPosition; }
    bool waiting() const { return m_Waiting; }
    int bitrateKbps() const { return m_BitrateKbps; }
    int minimumBitrateKbps() const;
    int maximumBitrateKbps() const;
    int bitrateStepKbps() const;
    QRectF videoRect() const;

    void setPresentation(const QtSessionGeometry& geometry);
    void setRemoteInputEnabled(bool enabled);
    void setRemoteCursor(const QCursor& cursor);
    void setPinned(bool pinned);
    void setWaiting(bool waiting);
    void setBitrateKbps(int kbps);
    Q_INVOKABLE void moveToolbar(qreal logicalLeft);
    Q_INVOKABLE void toggleFullscreen();
    Q_INVOKABLE void minimize();
    Q_INVOKABLE void disconnectSession();
    Q_INVOKABLE void keepWaiting();

signals:
    void toolbarChanged();
    void waitingChanged();
    void bitrateChanged();
    void presentationChanged();
    // Receiver recomputes the actual viewport before enabling input again.
    void surfaceSizeChanged();
    void remoteMotion(QPointF streamPosition);
    void remoteButton(int button, bool down, QPointF streamPosition);
    void remoteWheel(QPoint angleDelta, QPoint pixelDelta, bool inverted);
    void remoteKey(quint32 scanCode, int key, bool down, bool repeat, Qt::KeyboardModifiers modifiers);
    void bitrateRequested(int kbps);
    void disconnectRequested();
    void keepWaitingRequested();

protected:
    bool event(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    bool overToolbar(const QPointF& position) const;
    bool localPointer(const QPointF& position) const;
    void observePointer(const QPointF& position);
    void setToolbarVisible(bool visible);
    void updateCursor();
    void scheduleHide();
    void releaseRemoteInput();
    bool remoteKeyboard() const;
    static quint64 keyIdentity(const QKeyEvent* event);

    QtSessionGeometry m_Geometry;
    QPointer<QQuickItem> m_Toolbar;
    QTimer m_RevealTimer;
    QTimer m_HideTimer;
    QCursor m_RemoteCursor;
    QPointF m_Pointer;
    QPointF m_LastRemotePosition;
    Qt::MouseButtons m_RemoteButtons;
    Qt::MouseButtons m_LocalButtons;
    struct Key { quint32 scanCode; int key; Qt::KeyboardModifiers modifiers; };
    QMap<quint64, Key> m_RemoteKeys;
    bool m_RemoteInputEnabled = false;
    bool m_PointerPresent = false;
    bool m_ToolbarVisible = false;
    bool m_Pinned = false;
    bool m_Waiting = false;
    qreal m_ToolbarPosition = 0.5;
    int m_BitrateKbps;
};
