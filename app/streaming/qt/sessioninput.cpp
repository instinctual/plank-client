#include "sessioninput.h"
#include "../input/linuxwacom.h"
#include "../input/linuxrawwacom.h"
#include <QClipboard>
#include <QGuiApplication>
#include <QThread>
#include <algorithm>
#include <climits>
#include <memory>
#include <utility>

QtSessionInput::QtSessionInput(QObject* parent) : QObject(parent) {}
QtSessionInput::~QtSessionInput() { Q_ASSERT(!m_TabletThread.joinable()); }

void QtSessionInput::startTablets(unsigned hostFeatures)
{
    m_TabletThread = std::thread([this, hostFeatures] { tabletMain(hostFeatures); });
}

void QtSessionInput::tabletMain(unsigned hostFeatures)
{
    const auto activity = [this] { tabletActivity(); };
    std::unique_ptr<LinuxRawWacomInput> raw;
    std::unique_ptr<LinuxWacomInput> normalized;
    if (!qEnvironmentVariableIntValue("PLANK_EXTERNAL_WACOM_BRIDGE")) {
        const auto decision = plankWacomTransportForConnectedDevice();
        const auto rawFlags = LI_FF_RAW_HID_TABLET | LI_FF_RAW_HID_FOCUS_SUSPEND;
        if ((hostFeatures & rawFlags) == rawFlags && decision.transport == PlankWacomTransport::ExactRawHid)
            raw = std::make_unique<LinuxRawWacomInput>(activity);
        else if (hostFeatures & LI_FF_PEN_TOUCH_EVENTS)
            normalized = std::make_unique<LinuxWacomInput>(activity);
    }
    m_RawControl.publish(raw.get());
    TabletState previous = TabletState::Streaming;
    for (;;) {
        TabletState state;
        bool focused;
        std::uint64_t revision;
        {
            std::unique_lock<std::mutex> lock(m_TabletMutex);
            m_TabletChanged.wait(lock, [this] { return m_RequestedRevision != m_AppliedRevision; });
            state = m_TabletState; focused = m_TabletFocus; revision = m_RequestedRevision;
        }
        if (state != previous) {
            if (state == TabletState::Reconnecting) {
                if (raw) raw->beginReconnect();
                if (normalized) normalized->beginReconnect();
            } else if (state == TabletState::Streaming) {
                if (raw) raw->finishReconnect();
                if (normalized) normalized->finishReconnect();
            }
            previous = state;
        }
        if (raw) raw->setActive(focused && state == TabletState::Streaming);
        if (normalized) normalized->setActive(focused && state == TabletState::Streaming);
        if (state == TabletState::Stopped) break;
        {
            std::lock_guard<std::mutex> lock(m_TabletMutex);
            m_AppliedRevision = revision;
        }
        m_TabletChanged.notify_all();
    }
    m_RawControl.clear();
    raw.reset(); normalized.reset();
    {
        std::lock_guard<std::mutex> lock(m_TabletMutex);
        m_TabletFinished = true;
    }
    m_TabletChanged.notify_all();
}

void QtSessionInput::transition(TabletState state, bool wait)
{
    Q_ASSERT(!wait || QThread::currentThread() != thread());
    std::unique_lock<std::mutex> lock(m_TabletMutex);
    m_TabletState = state;
    const auto revision = ++m_RequestedRevision;
    m_TabletChanged.notify_all();
    if (wait && m_TabletThread.joinable())
        m_TabletChanged.wait(lock, [this, revision] { return m_AppliedRevision >= revision || m_TabletFinished; });
}
void QtSessionInput::beginReconnect() { transition(TabletState::Reconnecting, true); }
void QtSessionInput::finishReconnect() { transition(TabletState::Streaming, true); }
void QtSessionInput::shutdown()
{
    transition(TabletState::Stopped, true);
    if (m_TabletThread.joinable()) m_TabletThread.join();
}
void QtSessionInput::requestFocus()
{
    std::lock_guard<std::mutex> lock(m_TabletMutex);
    m_TabletFocus = m_Focused && m_Enabled;
    ++m_RequestedRevision;
    m_TabletChanged.notify_all();
}
void QtSessionInput::handleRawHidControl(const unsigned char* data, unsigned int length)
{
    m_RawControl.invoke([&](LinuxRawWacomInput& input) { input.handleControl(data, length); });
}
void QtSessionInput::handleCursorShape(const unsigned char* data, unsigned int length)
{
    if (m_Cursor.shape(data, length)) QMetaObject::invokeMethod(this, &QtSessionInput::consumeShape, Qt::QueuedConnection);
}
void QtSessionInput::handleCursorPosition(const unsigned char* data, unsigned int length)
{
    const auto size = m_StreamSize.load();
    if (m_Cursor.position(data, length, QSize(int(size >> 32), int(size & 0xffffffffu))))
        QMetaObject::invokeMethod(this, &QtSessionInput::consumePosition, Qt::QueuedConnection);
}
void QtSessionInput::resetCursorEpoch()
{
    m_Cursor.reset();
    m_ActivityPending.store(false);
    QMetaObject::invokeMethod(this, [this] {
        activateMouse();
        emit cursorShape({}, {}, false);
    }, Qt::QueuedConnection);
}
void QtSessionInput::consumeShape()
{
    const auto shape = m_Cursor.takeShape();
    if (!shape) return;
    const QImage image(shape->pixels.data(), int(shape->width), int(shape->height), int(shape->width * 4),
                       QImage::Format_ARGB32_Premultiplied);
    emit cursorShape(image.copy(), QPoint(int(shape->hotspotX), int(shape->hotspotY)), bool(shape->flags & PLANK_CURSOR_FLAG_VISIBLE));
}
void QtSessionInput::consumePosition()
{
    const auto position = m_Cursor.takePosition();
    if (position) emit cursorPosition(QPointF(position->x, position->y));
}
void QtSessionInput::tabletActivity()
{
    if (m_ActivityPending.exchange(true)) return;
    QMetaObject::invokeMethod(this, [this] {
        if (!m_ActivityPending.exchange(false) || !m_Enabled || !m_Focused || m_TabletActive) return;
        m_TabletActive = true;
        emit tabletActive(true);
    }, Qt::QueuedConnection);
}
void QtSessionInput::activateMouse()
{
    m_ActivityPending.store(false);
    if (!m_TabletActive) return;
    m_TabletActive = false;
    emit tabletActive(false);
}
void QtSessionInput::setStreamSize(QSize size)
{
    m_StreamSize.store((std::uint64_t(size.width()) << 32) | std::uint32_t(size.height()));
}
void QtSessionInput::setEnabled(bool enabled)
{
    if (m_Enabled == enabled) return;
    if (!enabled) { releaseAll(); activateMouse(); }
    m_Enabled = enabled;
    requestFocus();
}
void QtSessionInput::setFocused(bool focused)
{
    if (m_Focused == focused) return;
    if (!focused) { releaseAll(); activateMouse(); }
    m_Focused = focused;
    requestFocus();
}
bool QtSessionInput::sendPosition(QPointF position)
{
    const auto size = m_StreamSize.load();
    const int width = int(size >> 32), height = int(size & 0xffffffffu);
    if (width <= 0 || height <= 0 || width > SHRT_MAX || height > SHRT_MAX) return false;
    return LiSendMousePositionEvent(short(qBound(0, qRound(position.x()), width - 1)),
        short(qBound(0, qRound(position.y()), height - 1)), short(width), short(height)) == 0;
}
void QtSessionInput::mouseMotion(QPointF position)
{
    activateMouse();
    if (m_Enabled) sendPosition(position);
}
void QtSessionInput::mouseButton(int button, bool down, QPointF position)
{
    int wire;
    switch (button) {
    case Qt::LeftButton: wire = BUTTON_LEFT; break;
    case Qt::MiddleButton: wire = BUTTON_MIDDLE; break;
    case Qt::RightButton: wire = BUTTON_RIGHT; break;
    case Qt::BackButton: wire = BUTTON_X1; break;
    case Qt::ForwardButton: wire = BUTTON_X2; break;
    default: return;
    }
    activateMouse();
    if (down) {
        if (!m_Enabled || !sendPosition(position)) return;
        m_Buttons.insert(wire);
    } else if (!m_Buttons.remove(wire)) return;
    LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, wire);
}
void QtSessionInput::wheel(QPoint angleDelta, QPoint pixelDelta, bool inverted)
{
    Q_UNUSED(pixelDelta);
    Q_UNUSED(inverted); // Qt has already applied the compositor's natural-scroll preference.
    if (!m_Enabled) return;
    if (angleDelta.y()) LiSendHighResScrollEvent(short(qBound(int(SHRT_MIN), angleDelta.y(), int(SHRT_MAX))));
    if (angleDelta.x()) LiSendHighResHScrollEvent(short(qBound(int(SHRT_MIN), angleDelta.x(), int(SHRT_MAX))));
}
void QtSessionInput::key(quint32 scan, int key, bool down, bool repeat, Qt::KeyboardModifiers modifiers)
{
    if (repeat) return; // repeat is generated on the Host, as in the ordinary Client
    if (!down && m_ShortcutKeys.remove(scan)) return;
    if (down && m_Enabled && (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)) ==
            (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier)) {
        const auto physical = qtLinuxRemoteKey(scan).vk;
        const auto special = [](int code) { return code == Qt::Key_Q || code == Qt::Key_Z || code == Qt::Key_X ||
            code == Qt::Key_S || code == Qt::Key_D || code == Qt::Key_V || code == Qt::Key_L || code == Qt::Key_K; };
        const int shortcut = special(key) ? key : int(physical);
        if (special(shortcut)) { m_ShortcutKeys.insert(scan); releaseAll(); emit shortcutRequested(shortcut); return; }
    }
    auto mapped = qtLinuxRemoteKey(scan);
    if (down) {
        if (!m_Enabled || !mapped.vk || ((mapped.vk == 0x5b || mapped.vk == 0x5c) && !m_SystemKeys)) return;
        m_Keys.insert(scan, mapped);
    } else {
        auto held = m_Keys.find(scan);
        if (held == m_Keys.end()) return;
        mapped = held.value(); m_Keys.erase(held);
    }
    char flags = 0;
    if (modifiers & Qt::ControlModifier) flags |= MODIFIER_CTRL;
    if (modifiers & Qt::ShiftModifier) flags |= MODIFIER_SHIFT;
    if (modifiers & Qt::AltModifier) flags |= MODIFIER_ALT;
    if (m_SystemKeys && (modifiers & Qt::MetaModifier)) flags |= MODIFIER_META;
    LiSendKeyboardEvent2(short(0x8000 | mapped.vk), down ? KEY_ACTION_DOWN : KEY_ACTION_UP, flags,
                         mapped.nonNormalized ? SS_KBE_FLAG_NON_NORMALIZED : 0);
}
void QtSessionInput::releaseAll()
{
    for (const auto& key : std::as_const(m_Keys))
        LiSendKeyboardEvent2(short(0x8000 | key.vk), KEY_ACTION_UP, 0, key.nonNormalized ? SS_KBE_FLAG_NON_NORMALIZED : 0);
    m_Keys.clear();
    for (auto button : std::as_const(m_Buttons)) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
    m_Buttons.clear();
}
void QtSessionInput::pasteText()
{
    if (!m_Enabled) return;
    releaseAll();
    auto text = QGuiApplication::clipboard()->text().replace(QStringLiteral("\r\n"), QStringLiteral("\n")).toUtf8();
    if (text.size() <= 512 * 1024) LiSendUtf8TextEvent(text.constData(), unsigned(text.size()));
}
