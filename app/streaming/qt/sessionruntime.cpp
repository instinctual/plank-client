#include "sessionruntime.h"
#include "../session.h"
#include "../../backend/hostrecovery.h"
#include "../../backend/nvhttp.h"

#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <algorithm>
#include <tuple>

QtSessionRuntime::QtSessionRuntime(Session* session) : QObject(session), m_Session(session)
{
    connect(session, &Session::qtQuitRequested, this, &QtSessionRuntime::stop, Qt::QueuedConnection);
    connect(session, &Session::qtDecoderResetRequested, this, [this] { request(Command::ResetDecoder); }, Qt::QueuedConnection);
    connect(session, &Session::qtReconnectRequested, this, [this] { request(Command::Reconnect); }, Qt::QueuedConnection);
    connect(session, &Session::qtReconnectStatusChanged, this,
            [this](const QString& text, bool) { setStatus(text); }, Qt::QueuedConnection);
    m_Tick.setInterval(100);
    connect(&m_Tick, &QTimer::timeout, this, &QtSessionRuntime::tick);
    m_StartupDeadline.setSingleShot(true);
    m_StartupDeadline.setInterval(10000);
    connect(&m_StartupDeadline, &QTimer::timeout, this, [this] {
        emit m_Session->displayLaunchError(tr("The Qt video surface did not become ready."));
        stop();
    });
}

QtSessionRuntime::~QtSessionRuntime()
{
    // Session is retained until readyForDeletion, emitted only after this
    // worker has exited. Never join network or GPU work in the GUI destructor.
    Q_ASSERT(!m_Worker);
    QCoreApplication::instance()->removeEventFilter(this);
}

bool QtSessionRuntime::snapshotDisplays()
{
    const auto screens = QGuiApplication::screens();
    auto& session = *m_Session;
    session.m_ClientDisplays.clear();
    auto* target = m_Launcher ? m_Launcher->screen() : QGuiApplication::primaryScreen();
    for (int index = 0; index < screens.size(); ++index) {
        auto* screen = screens[index];
        const auto bounds = screen->geometry();
        const auto pixels = QSize(qRound(bounds.width() * screen->devicePixelRatio()),
                                  qRound(bounds.height() * screen->devicePixelRatio()));
        if (!bounds.isValid() || !pixels.isValid()) return false;
        Session::ClientDisplaySnapshot display;
        // Opaque snapshot identifier, not passed to any SDL display API.
        display.displayId = index + 1;
        display.logicalBounds = {bounds.x(), bounds.y(), bounds.width(), bounds.height()};
        display.nativeSize = pixels;
        display.primary = screen == QGuiApplication::primaryScreen();
        if (screen == target) session.m_TargetDisplayId = display.displayId;
        session.m_ClientDisplays.append(display);
    }
    auto& displays = session.m_ClientDisplays;
    if (displays.isEmpty()) return false;
    std::sort(displays.begin(), displays.end(), [](const auto& a, const auto& b) {
        return std::tie(a.logicalBounds.x, a.logicalBounds.y) < std::tie(b.logicalBounds.x, b.logicalBounds.y);
    });
    if (!session.m_TargetDisplayId) session.m_TargetDisplayId = displays.first().displayId;
    session.m_MultiDisplayPresentationAvailable = displays.size() == 2;
    if (displays.size() == 2) {
        const auto& a = displays[0].logicalBounds;
        const auto& b = displays[1].logicalBounds;
        session.m_MultiDisplayPresentationAvailable = a.x + a.w <= b.x && a.y < b.y + b.h && b.y < a.y + a.h;
    }
    int x = 0;
    for (auto& display : displays) {
        display.canvasRect = QRect(QPoint(x, 0), display.nativeSize);
        x += display.nativeSize.width();
    }
    return true;
}

void QtSessionRuntime::start(QWindow* launcher)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_Started) return;
    m_Started = true;
    m_Launcher = launcher;
    m_Session->m_QtWindow = launcher;
    m_Session->m_ThreadedExec = true;
    m_PreviousQuitOnLastWindow = QGuiApplication::quitOnLastWindowClosed();
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::instance()->installEventFilter(this);
    if (!snapshotDisplays()) {
        emit m_Session->displayLaunchError(tr("Unable to read the Client display layout."));
        stop(); return;
    }
    m_Presenter = std::make_unique<QtVideoPresenter>();
    const auto screens = QGuiApplication::screens();
    // Primary first: device creation and the A/V presentation clock use this
    // same output. Canvas coordinates retain physical left-to-right order.
    auto displays = m_Session->m_ClientDisplays;
    std::stable_sort(displays.begin(), displays.end(), [this](const auto& a, const auto& b) {
        return (a.displayId == m_Session->m_TargetDisplayId) > (b.displayId == m_Session->m_TargetDisplayId);
    });
    for (const auto& display : displays) {
        const bool primary = display.displayId == m_Session->m_TargetDisplayId;
        if (!primary && !m_Session->m_MultiDisplayPresentationAvailable) continue;
        auto shell = std::make_unique<QtSessionShell>();
        shell->setScreen(screens[int(display.displayId) - 1]);
        shell->setTitle(m_Session->m_Computer->name + QStringLiteral(" — PLANK Client Qt"));
        const auto available = shell->screen()->availableGeometry();
        shell->setGeometry(QRect(available.center() - QPoint(480, 300), QSize(960, 600)).intersected(available));
        auto& surface = shell->surface();
        surface.setLiveSession(true);
        surface.setStatusText(tr("Connecting to workstation…"));
        surface.setBitrateKbps(m_Session->m_PlankBitrateKbps);
        connect(&surface, &QtSessionWindow::disconnectRequested, this, &QtSessionRuntime::stop);
        connect(&surface, &QtSessionWindow::keepWaitingRequested, this, &QtSessionRuntime::keepWaiting);
        connect(&surface, &QtSessionWindow::surfaceSizeChanged, this, &QtSessionRuntime::updateGeometry);
        connect(shell.get(), &QWindow::windowStateChanged, this, [this] { updateGeometry(); });
        m_Presenter->addOutput(&surface, {}, primary);
        m_Windows.push_back(std::move(shell));
    }
    connect(m_Presenter.get(), &QtVideoPresenter::deviceReady, this, [this](bool available, const QString& reason) {
        m_StartupDeadline.stop();
        if (m_Stop.load()) return;
        if (!available) {
            emit m_Session->displayLaunchError(tr("No exact-format Qt video device is available: %1").arg(reason));
            stop(); return;
        }
        m_Factory = std::make_unique<QtDecodedVideoRendererFactory>(m_Presenter->softwareDevice(),
            m_Presenter->vaapiRenderNode(), m_Presenter->deliveryFactory());
        m_Worker = QThread::create([this] { workerMain(); });
        m_Worker->setObjectName(QStringLiteral("Qt Session Worker"));
        connect(m_Worker, &QThread::finished, this, &QtSessionRuntime::finishWorker);
        m_Worker->start();
    });
    connect(m_Presenter.get(), &QtVideoPresenter::presentationFailed, this, [this](const QString& reason) {
        emit m_Session->displayLaunchError(reason); stop();
    });
    for (auto& shell : m_Windows) {
        if (m_Session->m_IsFullScreen) shell->showFullScreen();
        else shell->show();
    }
    m_Tick.start();
    m_StartupDeadline.start();
    m_Presenter->initializeDevice();
}

bool QtSessionRuntime::eventFilter(QObject* object, QEvent* event)
{
    if (object == QCoreApplication::instance() && event->type() == QEvent::Quit && !m_Finished) {
        m_ExitApplication = true;
        stop();
        return true;
    }
    return QObject::eventFilter(object, event);
}

void QtSessionRuntime::request(Command command)
{
    if (m_Stop.load()) return;
    if (command == Command::Reconnect) {
        m_Streaming = false;
        for (auto& shell : m_Windows) shell->surface().setRemoteInputEnabled(false);
        keepWaiting();
    }
    {
        std::lock_guard<std::mutex> lock(m_CommandMutex);
        if (int(command) > int(m_Command)) m_Command = command;
    }
    m_CommandChanged.notify_all();
}

void QtSessionRuntime::stop()
{
    if (m_Stop.exchange(true)) return;
    m_Session->cancelConnectionStart();
    m_Session->m_ReconnectCancelled.store(true);
    m_Session->m_CanReconnect.store(false);
    m_StartupDeadline.stop();
    m_Tick.stop();
    for (auto& shell : m_Windows) { shell->surface().setRemoteInputEnabled(false); shell->hide(); }
    if (m_Presenter) m_Presenter->stop(); // also wakes test-frame qualification
    m_CommandChanged.notify_all();
    if (!m_Worker) QTimer::singleShot(0, this, &QtSessionRuntime::finishWorker);
}

bool QtSessionRuntime::resetDecoder()
{
    auto& s = *m_Session;
    SDL_LockSpinlock(&s.m_DecoderLock);
    delete s.m_VideoDecoder;
    s.m_VideoDecoder = nullptr;
    SDL_UnlockSpinlock(&s.m_DecoderLock);
    if (m_Stop.load()) return false;
    IVideoDecoder* decoder = nullptr;
    const bool ready = Session::chooseDecoder(DecoderSelectionMode::PreferExactHardwareThenSoftware,
        nullptr, s.m_ActiveVideoFormat, s.m_ActiveVideoWidth, s.m_ActiveVideoHeight,
        s.m_ActiveVideoFrameRate, true, false, decoder, s.isIdentityGbrEnabledForFormat(s.m_ActiveVideoFormat),
        s.decoderCaptureSource(), s.decoderEncoderBackend(), m_Factory.get());
    if (!ready || m_Stop.load()) { delete decoder; return false; }
    SDL_LockSpinlock(&s.m_DecoderLock);
    s.m_VideoDecoder = decoder;
    SDL_UnlockSpinlock(&s.m_DecoderLock);
    LiRequestIdrFrame();
    const QSize size(s.m_ActiveVideoWidth, s.m_ActiveVideoHeight);
    QMetaObject::invokeMethod(this, [this, size] { setStreaming(size); }, Qt::QueuedConnection);
    return true;
}

void QtSessionRuntime::workerMain()
{
    auto& s = *m_Session;
    for (;;) {
        if (m_Stop.load()) return;
        if (Session::s_ActiveSessionSemaphore.tryAcquire(1, 50)) break;
    }
    m_OwnsSession = true;
    Session::s_ActiveSession = &s;
    bool ready = !m_Stop.load() && s.initialize(m_Factory.get()) && !m_Stop.load() && s.startConnectionAsync() && !m_Stop.load();
    if (ready) ready = resetDecoder();
    if (!ready && !m_Stop.load()) emit s.displayLaunchError(tr("The Qt session could not start with the selected video format."));
    quint64 nextProbe = 0;
    while (ready && !m_Stop.load()) {
        Command command;
        {
            std::unique_lock<std::mutex> lock(m_CommandMutex);
            m_CommandChanged.wait_for(lock, std::chrono::milliseconds(100), [this] { return m_Stop.load() || m_Command != Command::None; });
            command = m_Command;
            m_Command = Command::None;
        }
        if (m_Stop.load()) break;
        if (command == Command::Reconnect) {
            Session::PlankReconnectState state;
            if (!s.beginPlankReconnect(state) || m_Stop.load()) break;
            const bool connected = s.runPlankReconnect() && !m_Stop.load();
            // Use the established recovery state machine. It requests a
            // decoder reset only if format/size changed; otherwise retain it.
            ready = s.finishPlankReconnect(connected, state);
            if (ready) {
                const QSize size(s.m_ActiveVideoWidth, s.m_ActiveVideoHeight);
                QMetaObject::invokeMethod(this, [this, size] { setStreaming(size); }, Qt::QueuedConnection);
            }
        } else if (command == Command::ResetDecoder) {
            ready = resetDecoder();
        }
        if (!ready || m_Stop.load()) break;
        const auto now = SDL_GetTicks();
        if (now >= nextProbe && !s.m_ReconnectRequested.load() && s.m_CanReconnect.load() &&
                PlankHostRecovery::videoSilent(now, s.m_LastPlankVideoReceived.load()) && !s.m_PlankWorkerInstance.isEmpty()) {
            nextProbe = now + PlankHostRecovery::ProbeIntervalMs;
            try {
                NvHTTP http(s.m_Computer->activeAddress);
                if (http.probeWorkerReplacement(s.m_PlankWorkerInstance, s.m_PlankHostCertificateSha256))
                    Session::clConnectionTerminated(-2);
            } catch (const GfeHttpResponseException&) {
            } catch (const QtNetworkReplyException&) {}
        }
    }
    // All shared media state is retired on this worker, before releasing the
    // active-session token or allowing another session to start.
    s.m_CanReconnect.store(false);
    s.m_ReconnectCancelled.store(true);
    s.stopPlankTransportMediaReceivers();
    SDL_LockSpinlock(&s.m_DecoderLock);
    delete s.m_VideoDecoder;
    s.m_VideoDecoder = nullptr;
    SDL_UnlockSpinlock(&s.m_DecoderLock);
    LiStopConnection();
    s.stopPlankTransportDataPlane();
    s.clearPlankReconnectCredentials();
    {
        QWriteLocker lock(&s.m_Computer->lock);
        s.m_Computer->sessionToken.fill(QChar('\0'));
        s.m_Computer->sessionToken.clear();
        s.m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
    }
}

void QtSessionRuntime::finishWorker()
{
    if (m_Finished) return;
    if (m_Worker && !m_Worker->wait(0)) {
        QTimer::singleShot(10, this, &QtSessionRuntime::finishWorker); return;
    }
    m_Finished = true;
    m_Stop.store(true);
    m_StartupDeadline.stop(); m_Tick.stop();
    delete m_Worker; m_Worker = nullptr;
    if (m_Presenter) m_Presenter->stop();
    m_Factory.reset();
    m_Presenter.reset();
    m_Windows.clear();
    if (m_OwnsSession) {
        m_Session->m_RendererFactory = nullptr;
        Session::s_ActiveSession = nullptr;
        Session::s_ActiveSessionSemaphore.release();
        m_OwnsSession = false;
    }
    QCoreApplication::instance()->removeEventFilter(this);
    QGuiApplication::setQuitOnLastWindowClosed(m_PreviousQuitOnLastWindow);
    // Do not access this after readyForDeletion: QML may collect Session.
    if (m_ExitApplication) QTimer::singleShot(0, QCoreApplication::instance(), &QCoreApplication::quit);
    const QPointer<Session> session(m_Session);
    emit session->sessionFinished();
    if (session) emit session->readyForDeletion();
}

void QtSessionRuntime::setStreaming(QSize size)
{
    if (m_Stop.load()) return;
    m_StreamSize = size;
    m_Streaming = true;
    m_ReconnectDeadline = 0;
    updateGeometry();
    setStatus({});
    for (auto& shell : m_Windows) {
        shell->surface().setWaiting(false);
        shell->surface().setRemoteInputEnabled(true);
    }
}

void QtSessionRuntime::updateGeometry()
{
    if (!m_Presenter || m_Windows.empty() || !m_StreamSize.isValid()) return;
    const bool fullscreen = m_Windows.front()->windowState() == Qt::WindowFullScreen;
    const bool multi = fullscreen && m_Windows.size() > 1;
    QSize canvas;
    if (multi) for (const auto& display : m_Session->m_ClientDisplays) {
        canvas.setWidth(qMax(canvas.width(), display.canvasRect.right() + 1));
        canvas.setHeight(qMax(canvas.height(), display.canvasRect.bottom() + 1));
    }
    for (size_t index = 0; index < m_Windows.size(); ++index) {
        auto& shell = *m_Windows[index];
        if (index && !multi) { shell.hide(); continue; }
        if (index && !shell.isVisible()) shell.showFullScreen();
        auto& surface = shell.surface();
        const QSize drawable(qRound(surface.width() * surface.devicePixelRatio()), qRound(surface.height() * surface.devicePixelRatio()));
        if (!drawable.isValid()) continue;
        QRect output(QPoint(), drawable);
        if (multi) for (const auto& display : m_Session->m_ClientDisplays) {
            const auto& b = display.logicalBounds;
            if (shell.screen() && shell.screen()->geometry() == QRect(b.x, b.y, b.w, b.h)) output = display.canvasRect;
        }
        const auto slice = PlankPresentation::sliceForDrawable(m_StreamSize, multi ? canvas : drawable, output, drawable);
        const QtSessionGeometry geometry{m_StreamSize, slice.sourceRect, slice.destinationRect, drawable, surface.size()};
        surface.setPresentation(geometry);
        m_Presenter->setGeometry(&surface, geometry);
    }
}

void QtSessionRuntime::setStatus(const QString& text)
{
    for (auto& shell : m_Windows) shell->surface().setStatusText(text);
}

void QtSessionRuntime::keepWaiting()
{
    m_ReconnectDeadline = SDL_GetTicks() + quint64(m_Session->m_Preferences->plankUnreachableTimeoutSeconds) * 1000;
    m_Session->m_ReconnectPolicy.allowUntil(m_ReconnectDeadline);
    for (auto& shell : m_Windows) shell->surface().setWaiting(false);
}

void QtSessionRuntime::tick()
{
    if (m_Stop.load()) return;
    if (m_Session->m_ReconnectGreeterConfirmed.exchange(false)) setStatus(tr("Returning to the sign-in screen…"));
    if (m_ReconnectDeadline && SDL_GetTicks() >= m_ReconnectDeadline) {
        m_ReconnectDeadline = 0;
        if (m_Session->m_Preferences->plankUnreachableAction == StreamingPreferences::PLANK_UNREACHABLE_DISCONNECT) stop();
        else for (auto& shell : m_Windows) shell->surface().setWaiting(true);
    }
    if (m_Streaming) {
        const bool silent = PlankHostRecovery::videoSilent(SDL_GetTicks(), m_Session->m_LastPlankVideoReceived.load());
        setStatus(silent ? tr("Waiting for workstation…") : QString());
    }
}
