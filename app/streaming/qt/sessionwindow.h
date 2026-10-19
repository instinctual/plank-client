#pragma once

#include "sessiongeometry.h"
#include "cursoritem.h"

#include <QCursor>
#include <QMap>
#include <QPointer>
#include <QQuickView>
#include <QTimer>
#include <QVariantMap>

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
    Q_PROPERTY(bool liveSession READ liveSession WRITE setLiveSession NOTIFY statusChanged)
    Q_PROPERTY(QString statusText READ statusText WRITE setStatusText NOTIFY statusChanged)
    Q_PROPERTY(QVariantMap telemetry READ telemetry WRITE setTelemetry NOTIFY telemetryChanged)
    Q_PROPERTY(QString statisticsText READ statisticsText WRITE setStatisticsText NOTIFY statisticsChanged)
    Q_PROPERTY(bool statisticsVisible READ statisticsVisible WRITE setStatisticsVisible NOTIFY statisticsChanged)
public:
    explicit QtSessionWindow(QWindow* parent = nullptr);
    ~QtSessionWindow() override;

    bool toolbarVisible() const { return m_ToolbarVisible; }
    bool pinned() const { return m_Pinned; }
    qreal toolbarPosition() const { return m_ToolbarPosition; }
    bool waiting() const { return m_Waiting; }
    int bitrateKbps() const { return m_BitrateKbps; }
    int minimumBitrateKbps() const;
    int maximumBitrateKbps() const;
    int bitrateStepKbps() const;
    QRectF videoRect() const;
    bool liveSession() const { return m_LiveSession; }
    QString statusText() const { return m_StatusText; }
    void setLiveSession(bool live) { m_LiveSession = live; emit statusChanged(); }
    void setStatusText(const QString& text) { if (text != m_StatusText) { m_StatusText = text; emit statusChanged(); } }
    QVariantMap telemetry() const { return m_Telemetry; }
    void setTelemetry(const QVariantMap& values) { if (m_Telemetry != values) { m_Telemetry = values; emit telemetryChanged(); } }
    QString statisticsText() const { return m_StatisticsText; }
    void setStatisticsText(const QString& text) { m_StatisticsText = text; emit statisticsChanged(); }
    bool statisticsVisible() const { return m_StatisticsVisible; }
    void setStatisticsVisible(bool visible) { m_StatisticsVisible = visible; emit statisticsChanged(); }
    void setCursorShape(QImage image, QPoint hotspot, bool visible);
    void setHostCursorPosition(QPointF position);
    void setTabletActive(bool active);
    void setEmbeddedCursor(bool embedded);

    void setPresentation(const QtSessionGeometry& geometry);
    void setRemoteInputEnabled(bool enabled);
    void setRemoteCursor(const QCursor& cursor);
    void setPinned(bool pinned);
    void setWaiting(bool waiting);
    void setBitrateKbps(int kbps);
    void cancelInput();
    Q_INVOKABLE void moveToolbar(qreal logicalLeft);
    Q_INVOKABLE void toggleFullscreen();
    Q_INVOKABLE void minimize();
    Q_INVOKABLE void disconnectSession();
    Q_INVOKABLE void keepWaiting();
    Q_INVOKABLE void toggleMicrophone() { emit microphoneRequested(); }
    Q_INVOKABLE void toggleCamera() { emit cameraRequested(); }
    Q_INVOKABLE void toggleStatistics() { setStatisticsVisible(!statisticsVisible()); }

signals:
    void statusChanged();
    void telemetryChanged();
    void statisticsChanged();
    void physicalMouseActivity();
    void microphoneRequested();
    void cameraRequested();
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
    void tabletEvent(QTabletEvent* event) override;

private:
    bool overToolbar(const QPointF& position) const;
    bool localPointer(const QPointF& position) const;
    void observePointer(const QPointF& position);
    void setToolbarVisible(bool visible);
    void updateCursor();
    void updateTabletCursor();
    void scheduleHide();
    void releaseRemoteInput();
    bool remoteKeyboard() const;
    static quint64 keyIdentity(const QKeyEvent* event);
    QWindow* controlWindow();

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
    bool m_LiveSession = false;
    QString m_StatusText;
    QVariantMap m_Telemetry;
    QString m_StatisticsText;
    bool m_StatisticsVisible = false;
    QtTabletCursorItem* m_TabletCursor = nullptr;
    QImage m_CursorImage;
    QPoint m_CursorHotspot;
    QPointF m_HostCursorPosition;
    bool m_HostPositionValid = false;
    bool m_HostCursorVisible = true;
    bool m_TabletActive = false;
    bool m_EmbeddedCursor = false;
};
