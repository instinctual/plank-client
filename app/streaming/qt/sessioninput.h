#pragma once
#include "../input/remotecursor.h"
#include "../input/penmetrics.h"
#include "../plankcallbacktarget.h"
#include "remotekeymap.h"
#include <QObject>
#include <QImage>
#include <QMap>
#include <QPointF>
#include <QSet>
#include <atomic>
#include <condition_variable>
#include <thread>

#ifdef Q_OS_MACOS
class MacRawWacomInput;
class MacKeyboardCapture;
class MacQuitShortcut;
using QtRawWacomInput = MacRawWacomInput;
#else
class LinuxRawWacomInput;
using QtRawWacomInput = LinuxRawWacomInput;
#endif

// Qt owns keys, buttons and cursor images on the GUI thread. Only lifecycle
// transitions of the existing tablet readers run on the tablet-control worker:
// their acknowledged focus suspension must never block GUI event delivery.
class QtSessionInput final : public QObject
{
    Q_OBJECT
public:
    explicit QtSessionInput(QObject* parent = nullptr);
    ~QtSessionInput() override;
    void startTablets(unsigned hostFeatures); // session worker, once connected
    // Set once before the tablet worker starts; callback accepts aggregates only.
    void setPenObserver(PlankPenObserver observer) { m_PenObserver = std::move(observer); }
    void beginReconnect();                  // session worker, acknowledged
    void finishReconnect();                 // session worker, acknowledged
    void shutdown();                        // session worker, joins tablet owner
    void resetCursorEpoch();                // after old receive callbacks stop
    void handleRawHidControl(const unsigned char* data, unsigned int length);
    void handleCursorShape(const unsigned char* data, unsigned int length);
    void handleCursorPosition(const unsigned char* data, unsigned int length);

    void setStreamSize(QSize size);
    void setEnabled(bool enabled);
    void setFocused(bool focused);
    void setSystemKeys(bool captured);
    void mouseMotion(QPointF position);
    void mouseButton(int button, bool down, QPointF position);
    void wheel(QPointF position, QPoint angleDelta, QPoint pixelDelta, bool inverted);
    void key(quint32 scan, int key, bool down, bool repeat, Qt::KeyboardModifiers modifiers);
    void releaseAll();
    void activateMouse();
    void pasteText();
#ifdef Q_OS_MACOS
    void normalizedPen(QPointF position, float pressure, float rotation, int xTilt, int yTilt, bool eraser, int buttons);
    bool capturesNativeKeys() const;
    void setNativeFocus(std::function<bool()> focused) { m_NativeFocus = std::move(focused); }
    void penLeave();
#endif

signals:
    void cursorShape(QImage image, QPoint hotspot, bool visible);
    void cursorPosition(QPointF position);
    void tabletActive(bool active);
    void shortcutRequested(int key);

private:
    enum class TabletState { Streaming, Reconnecting, Stopped };
    void transition(TabletState state, bool wait);
    void requestFocus();
    void tabletMain(unsigned hostFeatures);
    void tabletActivity();
    bool sendPosition(QPointF position, bool onlyIfChanged = false);
    void consumeShape();
    void consumePosition();

    PlankRemoteCursor m_Cursor;
    PlankCallbackTarget<QtRawWacomInput> m_RawControl;
    std::atomic<std::uint64_t> m_StreamSize{0};
    std::atomic_bool m_ActivityPending{false};
    // GUI-thread state: last successfully queued mouse position, in wire pixels.
    // A zero size invalidates it after focus, transport or tablet ownership changes.
    QPoint m_LastPosition;
    std::uint64_t m_LastPositionSize = 0;
    QMap<quint32, QtRemoteKey> m_Keys;
    QSet<int> m_Buttons;
    QSet<quint32> m_ShortcutKeys;
    bool m_Enabled = false, m_Focused = false, m_SystemKeys = false, m_TabletActive = false;
    std::mutex m_TabletMutex;
    std::condition_variable m_TabletChanged;
    TabletState m_TabletState = TabletState::Streaming;
    bool m_TabletFocus = false, m_TabletFinished = false;
    std::uint64_t m_RequestedRevision = 0, m_AppliedRevision = 0;
    std::thread m_TabletThread;
    PlankPenObserver m_PenObserver;
#ifdef Q_OS_MACOS
    std::unique_ptr<MacKeyboardCapture> m_MacKeys;
    std::unique_ptr<MacQuitShortcut> m_MacQuit;
    std::atomic_bool m_NormalizedPen{false};
    bool m_PenDown = false;
    bool m_PenPresent = false;
    std::function<bool()> m_NativeFocus;
#endif
};
