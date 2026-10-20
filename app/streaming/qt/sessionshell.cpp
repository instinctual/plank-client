#include "sessionshell.h"

#include <QCoreApplication>
#include <QPainter>

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

void QtSessionShell::resizeEvent(QResizeEvent* event)
{
    QRasterWindow::resizeEvent(event);
    m_Surface.setGeometry(QRect(QPoint(), size()));
}

void QtSessionShell::showEvent(QShowEvent* event)
{
    QRasterWindow::showEvent(event);
    m_Surface.setGeometry(QRect(QPoint(), size()));
    m_Surface.show();
}

void QtSessionShell::closeEvent(QCloseEvent* event)
{
    m_Surface.disconnectSession();
    QRasterWindow::closeEvent(event);
}
