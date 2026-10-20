#include "sessionshell.h"

#include <QCoreApplication>
#include <QPainter>
#include <QScreen>

void QtSessionShell::configureWaylandExposure()
{
    // The opaque video child can cover the raster parent completely. Wayland
    // may then omit the parent's frame callbacks while still presenting the
    // child. Qt 6.10's timeout heuristic hides that parent AND its children,
    // so video qualification/playback stops despite a visible, healthy output.
    //
    // Disable only that heuristic, not compositor callbacks or swap pacing.
    // Actual hide/minimize/exposure events and our bounded decoder gate remain.
    // This is process-local and called only by the opt-in Qt Client/probes.
    // Keep an explicit environment value for diagnostic A/B comparisons.
    if (!qEnvironmentVariableIsSet("QT_WAYLAND_FRAME_CALLBACK_TIMEOUT"))
        qputenv("QT_WAYLAND_FRAME_CALLBACK_TIMEOUT", "0");
}

QtSessionShell::QtSessionShell() : m_Surface(this)
{
    setTitle(QStringLiteral("PLANK Qt session qualification"));
    // Qt owns the parent/child Wayland relationship, all input dispatch and
    // native lifetime. There is no SDL window or application-owned wl_surface.
    connect(this, &QWindow::visibilityChanged, &m_Surface, [this](QWindow::Visibility visibility) {
        // Parent unmapping alone can leave Qt's child exposed flag stale.
        // This signal also covers programmatic minimize/restore, which need
        // not deliver WindowStateChange events to QWindow.
        if (visibility == QWindow::Hidden || visibility == QWindow::Minimized) {
            m_Surface.cancelInput();
            m_Surface.hide();
        } else {
            // Also covers backends that stay exposed while minimized and do
            // not send another expose on restore.
            queueSurfaceShow();
        }
    });
}

bool QtSessionShell::initialGeometryReady() const
{
    // GNOME may expose an initial normal configure before applying the queued
    // fullscreen configure. Exposure alone is not proof of fullscreen sizing.
    return !m_AwaitInitialFullscreenGeometry ||
        (screen() && size() == screen()->geometry().size());
}

void QtSessionShell::queueSurfaceShow()
{
    if (m_Surface.isVisible()) return;
    // Finish native configure/resize delivery before mapping the child. The
    // initial fullscreen gate also waits across separate normal/full configures.
    QMetaObject::invokeMethod(&m_Surface, [this] {
        if (m_Surface.isVisible() || !isVisible() || !isExposed() ||
                windowState() == Qt::WindowMinimized || !initialGeometryReady()) return;
        m_Surface.setGeometry(QRect(QPoint(), size()));
        m_AwaitInitialFullscreenGeometry = false;
        m_Surface.show();
    }, Qt::QueuedConnection);
}

QSize QtSessionShell::defaultWindowedSize(QSize available, QSize desktop)
{
    if (available.isEmpty()) return {};
    const QSize bounds(qMax(1, qRound(available.width() * 0.8)),
                       qMax(1, qRound(available.height() * 0.8)));
    if (desktop.isEmpty()) return bounds;
    return desktop.scaled(bounds, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
}

void QtSessionShell::prepareForSession(QSize desktop, bool fullscreen)
{
    Q_ASSERT(!isVisible());
    m_AwaitInitialFullscreenGeometry = fullscreen;
    const auto target = screen();
    if (target) {
        const QSize available = target->availableGeometry().isEmpty() ?
            target->geometry().size() : target->availableGeometry().size();
        // resize(), not setGeometry(): Wayland owns top-level placement.
        // These are logical pixels, not the stream's backing-pixel dimensions.
        resize(defaultWindowedSize(available, desktop));
    }
    // Create native resources while hidden and still in normal state, so Qt
    // records this restore size even if the first show is fullscreen. Creation
    // is not mapping: neither the shell nor its child is visible yet.
    create();
    // Native creation can reselect the screen containing the default (0,0)
    // position. Preserve the explicitly selected fullscreen output without
    // pretending we can position a Wayland top-level window ourselves.
    if (target) setScreen(target);
    m_WindowedSize = size();
    // Establish state before either surface is shown.
    setWindowState(fullscreen ? Qt::WindowFullScreen : Qt::WindowNoState);
}

void QtSessionShell::showSession(bool fullscreen)
{
    if (fullscreen) {
        // Retain content dimensions, not frame geometry or maximized size.
        // Qt Wayland's CSD transition can otherwise subtract the decoration
        // margins from its saved normal size on every fullscreen round trip.
        if (windowState() == Qt::WindowNoState) m_WindowedSize = size();
        m_RestoreWindowedSize = false;
        showFullScreen();
    } else {
        if (windowState() == Qt::WindowFullScreen)
            m_RestoreWindowedSize = !m_WindowedSize.isEmpty();
        showNormal();
    }
}

bool QtSessionShell::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::WindowStateChange:
        // Apply once, after the compositor acknowledges leaving fullscreen
        // and Qt has recreated the decorations. Resizing at request time is
        // too early on Wayland. No fixed border offset, timer or resize loop.
        if (m_RestoreWindowedSize && windowState() == Qt::WindowNoState) {
            m_RestoreWindowedSize = false;
            resize(m_WindowedSize);
        }
        break;
    case QEvent::Paint:
    case QEvent::UpdateRequest:
        // Do not map a smaller raster backing buffer while waiting for the
        // initial fullscreen configure. A later resize/expose resumes drawing.
        if (!initialGeometryReady()) {
            event->accept();
            return true;
        }
        break;
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::ShortcutOverride:
        // Wayland keyboard focus belongs to the top-level. Use the identical
        // scene input policy, including local reconnect-dialog controls.
        return QCoreApplication::sendEvent(&m_Surface, event);
    case QEvent::WindowDeactivate:
    case QEvent::Hide:
        m_Surface.cancelInput();
        break;
    default:
        break;
    }
    return QRasterWindow::event(event);
}

void QtSessionShell::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(QRect(QPoint(), size()), Qt::black);
}

void QtSessionShell::exposeEvent(QExposeEvent* event)
{
    QRasterWindow::exposeEvent(event);
    queueSurfaceShow();
    // Covered-parent exposure is not a reason to unmap the video child.
    // Explicit hide/minimize is handled above, independently of frame callbacks.
}

void QtSessionShell::resizeEvent(QResizeEvent* event)
{
    QRasterWindow::resizeEvent(event);
    m_Surface.setGeometry(QRect(QPoint(), size()));
    queueSurfaceShow();
}

void QtSessionShell::showEvent(QShowEvent* event)
{
    QRasterWindow::showEvent(event);
    m_Surface.setGeometry(QRect(QPoint(), size()));
}

void QtSessionShell::closeEvent(QCloseEvent* event)
{
    m_Surface.disconnectSession();
    QRasterWindow::closeEvent(event);
}
