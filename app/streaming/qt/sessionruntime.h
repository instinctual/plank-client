#pragma once

#include <QtGlobal>
#ifdef Q_OS_MACOS
#include "metalsessionshell.h"
using QtPlatformSessionShell = QtMetalSessionShell;
#else
#include "sessionshell.h"
#include "waylandsessionguard.h"
using QtPlatformSessionShell = QtSessionShell;
#endif
#include "videopresenter.h"
#include "decodedvideorenderer.h"
#include "sessioninput.h"
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

class Session;

// One GUI owner and one serialized session worker. Network/decoder startup,
// reconnect and teardown never run on Qt's GUI thread. No SDL video subsystem,
// native window, pointer warp or secondary event dispatcher is used here.
class QtSessionRuntime final : public QObject
{
    Q_OBJECT
public:
    explicit QtSessionRuntime(Session* session);
    ~QtSessionRuntime() override;
    void start(QWindow* launcher);

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    enum class Command { None, ResetDecoder, Reconnect, Stop };
    void request(Command command);
    void stop();
    void workerMain();
    void finishWorker();
    bool resetDecoder();
    void updateGeometry();
    void setStreaming(QSize size);
    void tick();
    void beginRecovery();
    void setStatus(const QString& text);
    bool snapshotDisplays();
    void updateFocus();
    void setInputEnabled(bool enabled);
    void handleShortcut(int key);
    void toggleFullscreen();
    void selectFullscreenDisplays(bool span, QtPlatformSessionShell* source);
    void showPresentationWindows();
    void minimize();

    Session* const m_Session;
    QPointer<QWindow> m_Launcher;
    std::vector<std::unique_ptr<QtPlatformSessionShell>> m_Windows;
    QList<QPointer<QScreen>> m_Screens;
    std::unique_ptr<QtVideoPresenter> m_Presenter;
    std::unique_ptr<QtDecodedVideoRendererFactory> m_Factory;
    std::unique_ptr<QtSessionInput> m_Input;
#ifdef Q_OS_LINUX
    std::unique_ptr<QtWaylandSessionGuard> m_WindowGuard;
#endif
    QThread* m_Worker = nullptr;
    QTimer m_Tick;
    QTimer m_StartupDeadline;
    QSize m_StreamSize;
    std::mutex m_CommandMutex;
    std::condition_variable m_CommandChanged;
    Command m_Command = Command::None;
    bool m_Closing = false; // GUI teardown is idempotent; cancellation lives in Session.
    bool m_OwnsSession = false; // worker-owned until joined
    bool m_Started = false;
    bool m_Finished = false;
    bool m_ExitApplication = false;
    bool m_PreviousQuitOnLastWindow = true;
    quint64 m_ReconnectNoticeDeadline = 0;
    quint64 m_BitrateChanged = 0, m_BitrateSent = 0;
    int m_LastSentBitrate = -1;
    int m_TargetBitrate = 0;
    std::atomic_int m_RecoveryBitrate{0};
    bool m_InputGrabbed = true;
    int m_CapturePolicy = 0;
    bool m_Fullscreen = true;
    bool m_SpanDisplays = false;
    bool m_ChangingPresentation = false;
    QString m_DisplayNotice;
    quint64 m_DisplayNoticeDeadline = 0;
    bool m_ConfinePointer = true;
};
