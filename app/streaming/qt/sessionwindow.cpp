#include "sessionwindow.h"
#include "sessionshortcut.h"
#include "../../settings/streamingpreferences.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QQmlContext>
#include <QQuickItem>
#include <QThread>
#include <QTabletEvent>

namespace {
constexpr int EdgeDwellMs = 1000;
constexpr int HideDelayMs = 5000;
constexpr int FullscreenRevealHeight = 3;
constexpr int WindowedRevealHeight = 12;
}

QtSessionWindow::QtSessionWindow(QWindow* parent) : QQuickView(parent),
    m_BitrateKbps(StreamingPreferences::PlankBitrateMinimumKbps)
{
    Q_ASSERT(thread() == QCoreApplication::instance()->thread());
    setTitle(QStringLiteral("PLANK Qt session qualification"));
    setResizeMode(QQuickView::SizeRootObjectToView);
    setColor(Qt::black);
    // A request only. The GPU integration gate must verify the actual swapchain.
    QSurfaceFormat surface = format();
    surface.setRedBufferSize(10);
    surface.setGreenBufferSize(10);
    surface.setBlueBufferSize(10);
    setFormat(surface);
    // Window chrome belongs to the top-level shell, not this native child.
    auto* window = controlWindow();
    m_Fullscreen = window->windowState() == Qt::WindowFullScreen;
    connect(window, &QWindow::windowStateChanged, this, [this](Qt::WindowState state) {
        const bool fullscreen = state == Qt::WindowFullScreen;
        if (m_Fullscreen == fullscreen) return;
        m_Fullscreen = fullscreen;
        m_RevealTimer.stop();
        emit fullscreenChanged();
    });
    rootContext()->setContextProperty(QStringLiteral("sessionWindow"), this);
    setSource(QUrl(QStringLiteral("qrc:/qt-session/SessionSurface.qml")));
    if (rootObject()) {
        m_Toolbar = rootObject()->findChild<QQuickItem*>(QStringLiteral("sessionToolbar"));
        m_TabletCursor = new QtTabletCursorItem(rootObject());
        m_TabletCursor->setVisible(false);
    }
    m_RevealTimer.setSingleShot(true);
    m_RevealTimer.setInterval(EdgeDwellMs);
    m_HideTimer.setSingleShot(true);
    m_HideTimer.setInterval(HideDelayMs);
    connect(&m_RevealTimer, &QTimer::timeout, this, [this] {
        if (m_PointerPresent && !m_Waiting && !m_RemoteButtons && !m_LocalButtons) {
            setToolbarVisible(true);
            scheduleHide();
        }
    });
    connect(&m_HideTimer, &QTimer::timeout, this, [this] {
        if (!m_Pinned && !m_LocalButtons &&
                (!m_PointerPresent || !overToolbar(m_Pointer))) {
            setToolbarVisible(false);
        }
    });
}

QtSessionWindow::~QtSessionWindow()
{
    releaseRemoteInput();
    // Retire QML while the derived object's properties and context still exist.
    m_TabletCursor = nullptr;
    setSource(QUrl());
}

QRectF QtSessionWindow::videoRect() const
{
    if (!m_Geometry.valid()) return {};
    const qreal sx = qreal(width()) / m_Geometry.drawableSize.width();
    const qreal sy = qreal(height()) / m_Geometry.drawableSize.height();
    const auto& d = m_Geometry.destinationPixels;
    return QRectF(d.x() * sx, d.y() * sy, d.width() * sx, d.height() * sy);
}

void QtSessionWindow::setPresentation(const QtSessionGeometry& geometry)
{
    Q_ASSERT(QThread::currentThread() == thread());
    releaseRemoteInput();
    m_Geometry = geometry.valid() && geometry.logicalSize == size() ? geometry : QtSessionGeometry{};
    emit presentationChanged();
    updateTabletCursor();
}

void QtSessionWindow::setRemoteInputEnabled(bool enabled)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!enabled) releaseRemoteInput();
    m_RemoteInputEnabled = enabled;
    updateCursor();
}

void QtSessionWindow::setRemoteCursor(const QCursor& cursor)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_RemoteCursor = cursor;
    updateCursor();
}

void QtSessionWindow::setCursorShape(QImage image, QPoint hotspot, bool visible)
{
    m_CursorImage = std::move(image);
    m_CursorHotspot = hotspot;
    m_HostCursorVisible = visible;
    if (m_TabletCursor) m_TabletCursor->setImage(m_CursorImage);
    if (m_CursorImage.isNull()) m_RemoteCursor = QCursor(Qt::ArrowCursor);
    else m_RemoteCursor = QCursor(QPixmap::fromImage(m_CursorImage), hotspot.x(), hotspot.y());
    updateCursor();
}
void QtSessionWindow::setHostCursorPosition(QPointF position)
{
    m_HostCursorPosition = position; m_HostPositionValid = true;
    updateTabletCursor();
}
void QtSessionWindow::setTabletActive(bool active)
{
    m_TabletActive = active;
    if (!active) m_HostPositionValid = false;
    updateCursor();
}
void QtSessionWindow::setEmbeddedCursor(bool embedded) { m_EmbeddedCursor = embedded; updateCursor(); }

void QtSessionWindow::updateTabletCursor()
{
    if (!m_TabletCursor) return;
    QPointF point;
    const bool visible = m_RemoteInputEnabled && !m_Waiting && m_TabletActive && !m_EmbeddedCursor &&
        m_HostCursorVisible && m_HostPositionValid && !m_CursorImage.isNull() &&
        m_Geometry.fromStream(m_HostCursorPosition, point);
    m_TabletCursor->setVisible(visible);
    if (!visible) return;
    const qreal sx = m_Geometry.destinationPixels.width() / m_Geometry.sourcePixels.width() * width() / m_Geometry.drawableSize.width();
    const qreal sy = m_Geometry.destinationPixels.height() / m_Geometry.sourcePixels.height() * height() / m_Geometry.drawableSize.height();
    m_TabletCursor->setPosition(point - QPointF(m_CursorHotspot.x() * sx, m_CursorHotspot.y() * sy));
    m_TabletCursor->setSize(QSizeF(m_CursorImage.width() * sx, m_CursorImage.height() * sy));
}

void QtSessionWindow::setPinned(bool pinned)
{
    if (m_Pinned == pinned) return;
    m_Pinned = pinned;
    if (pinned) {
        m_HideTimer.stop();
        setToolbarVisible(true);
    } else {
        scheduleHide();
    }
    emit toolbarChanged();
}

void QtSessionWindow::setWaiting(bool waiting)
{
    if (m_Waiting == waiting) return;
    cancelInput();
    m_Waiting = waiting;
    m_RevealTimer.stop();
    updateCursor();
    emit waitingChanged();
}

void QtSessionWindow::setBitrateKbps(int kbps)
{
    const int clamped = std::clamp(kbps, minimumBitrateKbps(), maximumBitrateKbps());
    const int step = bitrateStepKbps();
    const int quantized = ((clamped + step / 2) / step) * step;
    if (quantized == m_BitrateKbps) return;
    m_BitrateKbps = quantized;
    emit bitrateChanged();
    emit bitrateRequested(m_BitrateKbps);
}

int QtSessionWindow::minimumBitrateKbps() const { return StreamingPreferences::PlankBitrateMinimumKbps; }
int QtSessionWindow::maximumBitrateKbps() const { return StreamingPreferences::PlankBitrateMaximumKbps; }
int QtSessionWindow::bitrateStepKbps() const { return StreamingPreferences::PlankBitrateStepKbps; }

void QtSessionWindow::moveToolbar(qreal logicalLeft)
{
    if (!m_Toolbar || !std::isfinite(logicalLeft)) return;
    const qreal available = qMax(0.0, width() - m_Toolbar->width());
    m_ToolbarPosition = available > 0 ? std::clamp(logicalLeft / available, 0.0, 1.0) : 0.5;
    emit toolbarChanged();
}

void QtSessionWindow::toggleFullscreen()
{
    cancelInput();
    if (m_LiveSession) { emit fullscreenRequested(); return; }
    auto* window = controlWindow();
    if (window->windowState() == Qt::WindowFullScreen) window->showNormal();
    else window->showFullScreen();
}

void QtSessionWindow::minimize()
{
    cancelInput();
    if (m_LiveSession) { emit minimizeRequested(); return; }
    controlWindow()->showMinimized();
}

QWindow* QtSessionWindow::controlWindow()
{
    QWindow* window = this;
    while (window->parent()) window = window->parent();
    return window;
}

void QtSessionWindow::disconnectSession()
{
    setRemoteInputEnabled(false);
    emit disconnectRequested();
}

bool QtSessionWindow::overToolbar(const QPointF& position) const
{
    return m_Toolbar && m_Toolbar->isVisible() &&
            m_Toolbar->contains(m_Toolbar->mapFromScene(position));
}

bool QtSessionWindow::localPointer(const QPointF& position) const
{
    return m_LocalButtons || (!m_RemoteButtons && overToolbar(position));
}

void QtSessionWindow::setToolbarVisible(bool visible)
{
    if (m_ToolbarVisible == visible) return;
    m_ToolbarVisible = visible;
    emit toolbarChanged();
    updateCursor();
}

void QtSessionWindow::scheduleHide()
{
    if (m_Pinned || m_LocalButtons || (m_PointerPresent && overToolbar(m_Pointer))) {
        m_HideTimer.stop();
    } else if (m_ToolbarVisible && !m_HideTimer.isActive()) {
        m_HideTimer.start();
    }
}

void QtSessionWindow::observePointer(const QPointF& position)
{
    emit physicalMouseActivity();
    m_PointerPresent = true;
    m_Pointer = position;
    // The top of the *content*, including letterbox. Never Host pixels.
    // A decorated window cannot use the physical screen edge as a pointer
    // stop, so give it a more forgiving strip below the native title bar.
    const int revealHeight = m_Fullscreen ? FullscreenRevealHeight : WindowedRevealHeight;
    const bool edge = position.x() >= 0 && position.x() < width() &&
            position.y() >= 0 && position.y() < revealHeight;
    const bool reveal = edge && !m_Waiting &&
            !m_RemoteButtons && !m_LocalButtons;
    if (!reveal || m_ToolbarVisible) m_RevealTimer.stop();
    else if (!m_RevealTimer.isActive()) m_RevealTimer.start();
    scheduleHide();
    updateCursor();
}

void QtSessionWindow::updateCursor()
{
    QPointF ignored;
    const bool local = !m_RemoteInputEnabled || m_Waiting ||
        (!m_TabletActive && (localPointer(m_Pointer) || !m_Geometry.toStream(m_Pointer, ignored)));
    setCursor(local ? QCursor(Qt::ArrowCursor) :
        (m_TabletActive || m_EmbeddedCursor || !m_HostCursorVisible) ? QCursor(Qt::BlankCursor) : m_RemoteCursor);
    updateTabletCursor();
}

bool QtSessionWindow::event(QEvent* event)
{
    if (event->type() == QEvent::Leave) {
        m_PointerPresent = false;
        m_RevealTimer.stop();
        scheduleHide();
    } else if (event->type() == QEvent::ShortcutOverride && remoteKeyboard()) {
        event->accept();
        return true;
    } else if (event->type() == QEvent::Hide) {
        cancelInput();
    } else if (event->type() == QEvent::DevicePixelRatioChange) {
        releaseRemoteInput();
        m_Geometry = {};
        emit presentationChanged();
        emit surfaceSizeChanged();
    }
    return QQuickView::event(event);
}

void QtSessionWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (event->source() != Qt::MouseEventNotSynthesized) { event->accept(); return; }
    observePointer(event->position());
    QPointF position;
    if (m_RemoteInputEnabled && !m_Waiting && !localPointer(event->position()) &&
            mapPointer(event->position(), position, bool(m_RemoteButtons))) {
        m_LastRemotePosition = position;
        emit remoteMotion(position);
        event->accept();
        return;
    }
    QQuickView::mouseMoveEvent(event);
}

void QtSessionWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->source() != Qt::MouseEventNotSynthesized) { event->accept(); return; }
    observePointer(event->position());
    QPointF position;
    if (!m_RemoteInputEnabled && !m_Waiting && !localPointer(event->position()) &&
            m_Geometry.toStream(event->position(), position)) {
        // Recapturing is local: never forward this initial click to the Host.
        emit inputRecaptureRequested();
        event->accept();
        return;
    }
    if (m_RemoteInputEnabled && !m_Waiting && !localPointer(event->position()) &&
            m_Geometry.toStream(event->position(), position, bool(m_RemoteButtons))) {
        m_RemoteButtons |= event->button();
        m_LastRemotePosition = position;
        emit remoteButton(event->button(), true, position);
        event->accept();
        return;
    }
    m_LocalButtons |= event->button();
    m_RevealTimer.stop();
    m_HideTimer.stop();
    QQuickView::mousePressEvent(event);
}

void QtSessionWindow::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->source() != Qt::MouseEventNotSynthesized) { event->accept(); return; }
    // A double-click is also the second physical down, not a separate remote
    // command. Local controls still receive their ordinary Qt double-click.
    if (!localPointer(event->position()) && m_RemoteInputEnabled && !m_Waiting) {
        mousePressEvent(event);
    } else {
        QQuickView::mouseDoubleClickEvent(event);
    }
}

void QtSessionWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->source() != Qt::MouseEventNotSynthesized) { event->accept(); return; }
    observePointer(event->position());
    if (m_RemoteButtons.testFlag(event->button())) {
        QPointF position = m_LastRemotePosition;
        mapPointer(event->position(), position, true);
        m_RemoteButtons &= ~event->button();
        m_LastRemotePosition = position;
        emit remoteButton(event->button(), false, position);
        updateCursor();
        event->accept();
        return;
    }
    if (m_LocalButtons.testFlag(event->button())) {
        m_LocalButtons &= ~event->button();
        QQuickView::mouseReleaseEvent(event);
    } else {
        // A late release after focus/connection cancellation must not operate
        // a newly focused local control or create an unmatched remote up.
        event->accept();
    }
    scheduleHide();
    updateCursor();
}

void QtSessionWindow::wheelEvent(QWheelEvent* event)
{
    observePointer(event->position());
    QPointF position;
    if (m_RemoteInputEnabled && !m_Waiting && !localPointer(event->position()) &&
            m_Geometry.toStream(event->position(), position)) {
        // The input owner positions only when needed. Repeating absolute
        // motion before every notch switches the Linux Host between XTEST
        // and its wheel device, resetting applications' smooth-scroll state.
        emit remoteWheel(position, event->angleDelta(), event->pixelDelta(), event->inverted());
        event->accept();
        return;
    }
    QQuickView::wheelEvent(event);
}

bool QtSessionWindow::remoteKeyboard() const
{
    // Toolbar controls explicitly use NoFocus. Recovery dialog keys remain local.
    return m_RemoteInputEnabled && !m_Waiting;
}

quint64 QtSessionWindow::keyIdentity(const QKeyEvent* event)
{
    return event->nativeScanCode() ? (quint64(1) << 32) | event->nativeScanCode() : quint32(event->key());
}

void QtSessionWindow::keyPressEvent(QKeyEvent* event)
{
    if (!remoteKeyboard()) {
        const int shortcut = qtSessionShortcut(event->nativeScanCode(), event->key(), event->modifiers());
        // Disconnect is local even during startup, released input or a Host
        // timeout. It must not depend on a healthy remote input channel.
        if (shortcut == Qt::Key_Q || (!m_Waiting && shortcut == Qt::Key_Z)) {
            if (!event->isAutoRepeat()) {
                if (shortcut == Qt::Key_Q) disconnectSession();
                else emit inputRecaptureRequested();
            }
            event->accept();
            return;
        }
        QQuickView::keyPressEvent(event);
        return;
    }
    const auto id = keyIdentity(event);
    if (!m_RemoteKeys.contains(id) && event->isAutoRepeat()) {
        event->accept();
        return;
    }
    m_RemoteKeys.insert(id, {event->nativeScanCode(), event->key(), event->modifiers()});
    emit remoteKey(event->nativeScanCode(), event->key(), true, event->isAutoRepeat(), event->modifiers());
    event->accept();
}

void QtSessionWindow::keyReleaseEvent(QKeyEvent* event)
{
    const auto id = keyIdentity(event);
    if (m_RemoteKeys.contains(id)) {
        // X11 auto-repeat can synthesize release/press pairs; held ownership
        // lasts until the real release on both X11 and Wayland.
        if (!event->isAutoRepeat()) {
            m_RemoteKeys.remove(id);
            emit remoteKey(event->nativeScanCode(), event->key(), false, false, event->modifiers());
        }
        event->accept();
    } else if (!remoteKeyboard()) {
        QQuickView::keyReleaseEvent(event);
    } else {
        event->accept();
    }
}

void QtSessionWindow::releaseRemoteInput()
{
    const auto buttons = m_RemoteButtons;
    m_RemoteButtons = {};
    for (unsigned bit = 1; bit <= unsigned(Qt::MaxMouseButton); bit <<= 1) {
        if (buttons.testFlag(Qt::MouseButton(bit))) emit remoteButton(int(bit), false, m_LastRemotePosition);
    }
    const auto keys = std::move(m_RemoteKeys);
    m_RemoteKeys.clear();
    for (const auto& key : keys) emit remoteKey(key.scanCode, key.key, false, false, key.modifiers);
}

void QtSessionWindow::cancelInput()
{
    Q_ASSERT(QThread::currentThread() == thread());
    releaseRemoteInput();
    if (auto* grabber = mouseGrabberItem()) grabber->ungrabMouse();
    m_LocalButtons = {};
    m_PointerPresent = false;
    m_RevealTimer.stop();
    scheduleHide();
}

void QtSessionWindow::focusOutEvent(QFocusEvent* event)
{
    cancelInput();
    QQuickView::focusOutEvent(event);
}

void QtSessionWindow::resizeEvent(QResizeEvent* event)
{
    releaseRemoteInput();
    m_Geometry = {};
    QQuickView::resizeEvent(event);
    emit presentationChanged();
    emit surfaceSizeChanged();
}

void QtSessionWindow::closeEvent(QCloseEvent* event)
{
    disconnectSession();
    QQuickView::closeEvent(event);
}

void QtSessionWindow::tabletEvent(QTabletEvent* event)
{
    // The existing raw-HID/normalized readers own tablet forwarding. Qt's
    // tablet event must not synthesize a duplicate mouse click or pressure.
    event->accept();
}

bool QtSessionWindow::mapPointer(QPointF logical, QPointF& stream, bool drag) const
{
    if (drag && m_DragMapper) return m_DragMapper(logical, stream);
    return m_Geometry.toStream(logical, stream, drag);
}
