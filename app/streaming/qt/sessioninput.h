#pragma once
#include "../input/remotecursor.h"
#include "../plankcallbacktarget.h"
#include "linuxkeymap.h"
#include <QObject>
#include <QImage>
#include <QMap>
#include <QPointF>
#include <QSet>
#include <atomic>
#include <condition_variable>
#include <thread>

class LinuxRawWacomInput;

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
    void setSystemKeys(bool captured) { m_SystemKeys = captured; }
    void mouseMotion(QPointF position);
    void mouseButton(int button, bool down, QPointF position);
    void wheel(QPoint angleDelta, QPoint pixelDelta, bool inverted);
    void key(quint32 scan, int key, bool down, bool repeat, Qt::KeyboardModifiers modifiers);
    void releaseAll();
    void activateMouse();
    void pasteText();

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
    bool sendPosition(QPointF position);
    void consumeShape();
    void consumePosition();

    PlankRemoteCursor m_Cursor;
    PlankCallbackTarget<LinuxRawWacomInput> m_RawControl;
    std::atomic<std::uint64_t> m_StreamSize{0};
    std::atomic_bool m_ActivityPending{false};
    std::atomic_bool m_LocalCursor{false};
    QMap<quint32, QtLinuxRemoteKey> m_Keys;
    QSet<int> m_Buttons;
    QSet<quint32> m_ShortcutKeys;
    bool m_Enabled = false, m_Focused = false, m_SystemKeys = false, m_TabletActive = false;
    std::mutex m_TabletMutex;
    std::condition_variable m_TabletChanged;
    TabletState m_TabletState = TabletState::Streaming;
    bool m_TabletFocus = false, m_TabletFinished = false;
    std::uint64_t m_RequestedRevision = 0, m_AppliedRevision = 0;
    std::thread m_TabletThread;
};
