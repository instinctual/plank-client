#include "waylandsessionguard.h"
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QTimer>
#include <QWindow>
#include <QWaylandClientExtensionTemplate>
#include <qpa/qplatformnativeinterface.h>
#include "qwayland-keyboard-shortcuts-inhibit-unstable-v1.h"
#include "qwayland-pointer-constraints-unstable-v1.h"
#include "qwayland-idle-inhibit-unstable-v1.h"

namespace {
// These events are advisory. Input/focus ownership remains with Qt, but every
// advertised event must have a listener on Qt's dispatch queue.
const zwp_keyboard_shortcuts_inhibitor_v1_listener KeyboardListener = {
    [](void*, zwp_keyboard_shortcuts_inhibitor_v1*) {},
    [](void*, zwp_keyboard_shortcuts_inhibitor_v1*) {}
};
const zwp_confined_pointer_v1_listener PointerListener = {
    [](void*, zwp_confined_pointer_v1*) {},
    [](void*, zwp_confined_pointer_v1*) {}
};
class KeyboardManager final : public QWaylandClientExtensionTemplate<KeyboardManager>,
    public QtWayland::zwp_keyboard_shortcuts_inhibit_manager_v1
{
public:
    KeyboardManager() : QWaylandClientExtensionTemplate(1) {}
    ~KeyboardManager() override { if (isInitialized()) destroy(); }
};
class PointerManager final : public QWaylandClientExtensionTemplate<PointerManager>,
    public QtWayland::zwp_pointer_constraints_v1
{
public:
    PointerManager() : QWaylandClientExtensionTemplate(1) {}
    ~PointerManager() override { if (isInitialized()) destroy(); }
};
class IdleManager final : public QWaylandClientExtensionTemplate<IdleManager>,
    public QtWayland::zwp_idle_inhibit_manager_v1
{
public:
    IdleManager() : QWaylandClientExtensionTemplate(1) {}
    ~IdleManager() override { if (isInitialized()) destroy(); }
};
wl_surface* surface(QWindow* window)
{
    if (!window || !window->isExposed() || !window->handle()) return nullptr;
    return static_cast<wl_surface*>(QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window));
}
}
struct QtWaylandSessionGuard::Impl {
    KeyboardManager keyboard;
    PointerManager pointer;
    IdleManager idle;
    zwp_keyboard_shortcuts_inhibitor_v1* inhibitor = nullptr;
    zwp_confined_pointer_v1* confined = nullptr;
    zwp_idle_inhibitor_v1* idleInhibitor = nullptr;
};

QtWaylandSessionGuard::QtWaylandSessionGuard(QObject* parent) : QObject(parent)
{
    if (!qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>()) return;
    m_Impl = std::make_unique<Impl>();
    connect(&m_Impl->keyboard, &QWaylandClientExtension::activeChanged, this, &QtWaylandSessionGuard::refresh);
    connect(&m_Impl->pointer, &QWaylandClientExtension::activeChanged, this, &QtWaylandSessionGuard::refresh);
    connect(&m_Impl->idle, &QWaylandClientExtension::activeChanged, this, &QtWaylandSessionGuard::refresh);
}
QtWaylandSessionGuard::~QtWaylandSessionGuard()
{
    for (const auto& window : m_Watched) if (window) window->removeEventFilter(this);
    release();
    if (m_Impl && m_Impl->idleInhibitor) zwp_idle_inhibitor_v1_destroy(m_Impl->idleInhibitor);
}
void QtWaylandSessionGuard::release()
{
    if (!m_Impl) return;
    if (m_Impl->inhibitor) zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_Impl->inhibitor);
    if (m_Impl->confined) zwp_confined_pointer_v1_destroy(m_Impl->confined);
    m_Impl->inhibitor = nullptr; m_Impl->confined = nullptr;
}
void QtWaylandSessionGuard::setWindows(QWindow* keyboard, QWindow* pointer)
{
    if (m_KeyboardWindow == keyboard && m_PointerWindow == pointer) return;
    release();
    m_KeyboardWindow = keyboard; m_PointerWindow = pointer;
    watchWindows();
    refresh();
}
void QtWaylandSessionGuard::watchWindows()
{
    for (const auto& window : m_Watched) if (window) window->removeEventFilter(this);
    m_Watched.clear();
    for (const auto& window : {m_KeyboardWindow, m_PointerWindow, m_IdleWindow})
        if (window && !m_Watched.contains(window)) { window->installEventFilter(this); m_Watched.append(window); }
}
void QtWaylandSessionGuard::setIdleWindow(QWindow* window)
{
    if (m_IdleWindow == window) return;
    if (m_Impl && m_Impl->idleInhibitor) {
        zwp_idle_inhibitor_v1_destroy(m_Impl->idleInhibitor); m_Impl->idleInhibitor = nullptr;
    }
    m_IdleWindow = window;
    watchWindows(); refresh();
}
void QtWaylandSessionGuard::setPolicy(bool captureKeys, bool confinePointer)
{
    m_CaptureKeys = captureKeys; m_Confine = confinePointer;
    refresh();
}
bool QtWaylandSessionGuard::eventFilter(QObject* object, QEvent* event)
{
    const bool disappearing = event->type() == QEvent::Hide || (event->type() == QEvent::PlatformSurface &&
        static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
    if (disappearing && object == m_IdleWindow && m_Impl && m_Impl->idleInhibitor) {
        zwp_idle_inhibitor_v1_destroy(m_Impl->idleInhibitor); m_Impl->idleInhibitor = nullptr;
    }
    if (event->type() == QEvent::PlatformSurface) {
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
            release();
        else QTimer::singleShot(0, this, &QtWaylandSessionGuard::refresh);
    } else if (event->type() == QEvent::Hide) release();
    else if (event->type() == QEvent::Expose) refresh();
    return false;
}
void QtWaylandSessionGuard::refresh()
{
    if (!m_Impl) return;
    auto* native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!native) { release(); return; }
    auto* keyboardSurface = surface(m_KeyboardWindow);
    auto* pointerSurface = surface(m_PointerWindow);
    const bool keys = m_CaptureKeys && keyboardSurface && native->seat() && m_Impl->keyboard.isActive();
    const bool pointer = m_Confine && pointerSurface && native->pointer() && m_Impl->pointer.isActive();
    auto* idleSurface = surface(m_IdleWindow);
    if ((!idleSurface || !m_Impl->idle.isActive()) && m_Impl->idleInhibitor) {
        zwp_idle_inhibitor_v1_destroy(m_Impl->idleInhibitor); m_Impl->idleInhibitor = nullptr;
    }
    if (idleSurface && m_Impl->idle.isActive() && !m_Impl->idleInhibitor)
        m_Impl->idleInhibitor = m_Impl->idle.create_inhibitor(idleSurface);
    if (!keys && m_Impl->inhibitor) {
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_Impl->inhibitor); m_Impl->inhibitor = nullptr;
    }
    if (!pointer && m_Impl->confined) {
        zwp_confined_pointer_v1_destroy(m_Impl->confined); m_Impl->confined = nullptr;
    }
    if (keys && !m_Impl->inhibitor) {
        m_Impl->inhibitor = m_Impl->keyboard.inhibit_shortcuts(keyboardSurface, native->seat());
        zwp_keyboard_shortcuts_inhibitor_v1_add_listener(m_Impl->inhibitor, &KeyboardListener, nullptr);
    }
    if (pointer && !m_Impl->confined) {
        m_Impl->confined = m_Impl->pointer.confine_pointer(pointerSurface, native->pointer(), nullptr,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_confined_pointer_v1_add_listener(m_Impl->confined, &PointerListener, nullptr);
    }
}
