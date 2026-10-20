#pragma once

#include "sessionwindow.h"

#include <QRasterWindow>

// Ordinary Qt window chrome around a Qt-owned native scene. The scene contains
// video and controls together, but never passes through Qt Wayland's RGB8
// decoration framebuffer. The shell paints only on expose/resize, not per frame.
// This remains an isolated Ubuntu prototype, not a shipping session backend.
class QtSessionShell final : public QRasterWindow
{
public:
    // Call before QGuiApplication creates any Wayland surfaces.
    static void configureWaylandExposure();
    static QSize defaultWindowedSize(QSize available, QSize desktop);
    QtSessionShell();
    QtSessionWindow& surface() { return m_Surface; }
    void prepareForSession(QSize desktop, bool fullscreen);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    QtSessionWindow m_Surface;
};
