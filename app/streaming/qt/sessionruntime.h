#pragma once

#include "sessionshell.h"
#include "videopresenter.h"
#include "decodedvideorenderer.h"
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
    void keepWaiting();
    void setStatus(const QString& text);
    bool snapshotDisplays();

    Session* const m_Session;
    QPointer<QWindow> m_Launcher;
    std::vector<std::unique_ptr<QtSessionShell>> m_Windows;
    std::unique_ptr<QtVideoPresenter> m_Presenter;
    std::unique_ptr<QtDecodedVideoRendererFactory> m_Factory;
    QThread* m_Worker = nullptr;
    QTimer m_Tick;
    QTimer m_StartupDeadline;
    QSize m_StreamSize;
    std::mutex m_CommandMutex;
    std::condition_variable m_CommandChanged;
    Command m_Command = Command::None;
    std::atomic_bool m_Stop{false};
    bool m_OwnsSession = false; // worker-owned until joined
    bool m_Streaming = false;
    bool m_Started = false;
    bool m_Finished = false;
    bool m_ExitApplication = false;
    bool m_PreviousQuitOnLastWindow = true;
    quint64 m_ReconnectDeadline = 0;
};
