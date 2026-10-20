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
        } else if (isExposed()) {
            // Some platform backends keep the parent exposed while minimized
            // and do not send another expose on restore.
            m_Surface.setGeometry(QRect(QPoint(), size()));
            m_Surface.show();
        }
        // Show only after the shell's expose, when the compositor has supplied
        // its actual size. visibilityChanged precedes initial native creation;
        // showing the child here can expose its small pre-fullscreen geometry.
    });
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
    // Establish state before either surface is shown. Qt then retains normal
    // user resizes across toggles; no application-owned resize loop is needed.
    setWindowState(fullscreen ? Qt::WindowFullScreen : Qt::WindowNoState);
}

bool QtSessionShell::event(QEvent* event)
{
    switch (event->type()) {
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
    if (isExposed() && isVisible() && windowState() != Qt::WindowMinimized) {
        m_Surface.setGeometry(QRect(QPoint(), size()));
        m_Surface.show();
    }
    // Covered-parent exposure is not a reason to unmap the video child.
    // Explicit hide/minimize is handled above, independently of frame callbacks.
}

void QtSessionShell::resizeEvent(QResizeEvent* event)
{
    QRasterWindow::resizeEvent(event);
    m_Surface.setGeometry(QRect(QPoint(), size()));
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
