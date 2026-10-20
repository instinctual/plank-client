#include "sessionruntime.h"
#include "../session.h"
#include "../../backend/hostrecovery.h"
#include "../../backend/nvhttp.h"
#include "../audio/microphone.h"
#include "../camera/camera.h"

#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <algorithm>
#include <tuple>

QtSessionRuntime::QtSessionRuntime(Session* session) : QObject(session), m_Session(session)
{
    m_Input = std::make_unique<QtSessionInput>();
    m_WindowGuard = std::make_unique<QtWaylandSessionGuard>();
    m_TargetBitrate = session->m_PlankBitrateKbps;
    m_CapturePolicy = session->m_Preferences->captureSysKeysMode;
    m_Fullscreen = session->m_IsFullScreen;
    m_RecoveryBitrate.store(m_TargetBitrate);
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this] { updateFocus(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* removed) {
        if (m_Screens.contains(removed)) {
            emit m_Session->displayLaunchError(tr("A session display was disconnected. Reconnect using the current Client display layout."));
            stop(); return;
        }
    });
    connect(m_Input.get(), &QtSessionInput::shortcutRequested, this, &QtSessionRuntime::handleShortcut);
    connect(session, &Session::qtStatisticsUpdated, this, [this](const QString& text) {
        for (auto& shell : m_Windows) shell->surface().setStatisticsText(text);
    });
    connect(session, &Session::activeSessionTakeoverRequested, this, [this] {
        // The existing launcher owns the authenticated takeover dialog. Never
        // bury it under a fullscreen stream surface while startup is waiting.
        for (auto& shell : m_Windows) shell->hide();
        if (m_Launcher) { m_Launcher->show(); m_Launcher->raise(); m_Launcher->requestActivate(); }
    });
    connect(session, &Session::connectionStarted, this, [this] {
        if (m_Stop.load()) return;
        for (size_t i = 0; i < m_Windows.size(); ++i) {
            if (m_Fullscreen) m_Windows[i]->showFullScreen();
            else if (!i) m_Windows[i]->show();
        }
    });
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
    QSize desktop;
    {
        QReadLocker lock(&m_Session->m_Computer->lock);
        desktop = QSize(m_Session->m_Computer->outputTopology.desktopWidth,
                        m_Session->m_Computer->outputTopology.desktopHeight);
    }
    std::stable_sort(displays.begin(), displays.end(), [this](const auto& a, const auto& b) {
        return (a.displayId == m_Session->m_TargetDisplayId) > (b.displayId == m_Session->m_TargetDisplayId);
    });
    for (const auto& display : displays) {
        const bool primary = display.displayId == m_Session->m_TargetDisplayId;
        if (!primary && !m_Session->m_MultiDisplayPresentationAvailable) continue;
        auto shell = std::make_unique<QtSessionShell>();
        shell->setScreen(screens[int(display.displayId) - 1]);
        m_Screens.append(shell->screen());
        shell->setTitle(m_Session->m_Computer->name + QStringLiteral(" — PLANK Client Qt"));
        shell->prepareForSession(desktop, m_Fullscreen);
        auto& surface = shell->surface();
        surface.setLiveSession(true);
        surface.setStatusText(tr("Connecting to workstation…"));
        surface.setBitrateKbps(m_Session->m_PlankBitrateKbps);
        surface.setPinned(m_Session->m_Preferences->plankToolbarPinned);
        surface.setStatisticsVisible(m_Session->m_Preferences->showPerformanceOverlay);
        surface.setDragMapper([this, &surface](QPointF logical, QPointF& stream) {
            if (!m_Fullscreen || m_Windows.size() < 2 || !surface.screen())
                return surface.presentation().toStream(logical, stream, true);
            QVector<QtSessionOutputGeometry> outputs;
            for (const auto& window : m_Windows) {
                if (window->isVisible() && window->screen())
                    outputs.append({QRectF(window->screen()->geometry().topLeft(), window->surface().size()),
                                    window->surface().presentation()});
            }
            return qtMapDraggedPointer(outputs, QPointF(surface.screen()->geometry().topLeft()) + logical, stream);
        });
        connect(&surface, &QtSessionWindow::physicalMouseActivity, m_Input.get(), &QtSessionInput::activateMouse);
        connect(&surface, &QtSessionWindow::remoteMotion, m_Input.get(), &QtSessionInput::mouseMotion);
        connect(&surface, &QtSessionWindow::remoteButton, m_Input.get(), &QtSessionInput::mouseButton);
        connect(&surface, &QtSessionWindow::remoteWheel, m_Input.get(), &QtSessionInput::wheel);
        connect(&surface, &QtSessionWindow::remoteKey, m_Input.get(), &QtSessionInput::key);
        connect(m_Input.get(), &QtSessionInput::cursorShape, &surface, &QtSessionWindow::setCursorShape);
        connect(m_Input.get(), &QtSessionInput::cursorPosition, &surface, &QtSessionWindow::setHostCursorPosition);
        connect(m_Input.get(), &QtSessionInput::tabletActive, &surface, &QtSessionWindow::setTabletActive);
        connect(&surface, &QtSessionWindow::toolbarChanged, this, [&surface, this] {
            auto& preferences = *m_Session->m_Preferences;
            if (preferences.plankToolbarPinned != surface.pinned()) {
                preferences.plankToolbarPinned = surface.pinned(); preferences.save();
                for (auto& window : m_Windows) window->surface().setPinned(surface.pinned());
            }
        });
        connect(&surface, &QtSessionWindow::bitrateRequested, this, [this](int bitrate) {
            if (bitrate == m_TargetBitrate) return;
            m_TargetBitrate = bitrate; m_RecoveryBitrate.store(bitrate);
            m_BitrateChanged = SDL_GetTicks();
            for (auto& window : m_Windows) window->surface().setBitrateKbps(bitrate);
        });
        connect(&surface, &QtSessionWindow::microphoneRequested, this, [this] {
            m_Session->m_MicrophoneRequested.store(!m_Session->m_MicrophoneRequested.load());
        });
        connect(&surface, &QtSessionWindow::cameraRequested, this, [this] {
            m_Session->m_CameraRequested.store(!m_Session->m_CameraRequested.load());
        });
        connect(&surface, &QtSessionWindow::disconnectRequested, this, &QtSessionRuntime::stop);
        connect(&surface, &QtSessionWindow::fullscreenRequested, this, &QtSessionRuntime::toggleFullscreen);
        connect(&surface, &QtSessionWindow::minimizeRequested, this, &QtSessionRuntime::minimize);
        connect(&surface, &QtSessionWindow::inputRecaptureRequested, this, [this] {
            if (!m_Streaming || m_Stop.load() || m_ReconnectNoticeDeadline) return;
            m_InputGrabbed = true;
            setInputEnabled(true);
        });
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
    for (size_t i = 0; i < m_Windows.size(); ++i) {
        if (m_Fullscreen) m_Windows[i]->showFullScreen();
        else if (!i) m_Windows[i]->show();
    }
    m_Tick.start();
    m_WindowGuard->setIdleWindow(&m_Windows.front()->surface());
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
        setInputEnabled(false);
        beginRecovery();
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
    m_Session->m_ReconnectPolicy.allowUntil(0);
    m_Session->cancelConnectionStart();
    m_Session->m_ReconnectCancelled.store(true);
    m_Session->m_CanReconnect.store(false);
    m_StartupDeadline.stop();
    m_Tick.stop();
    setInputEnabled(false);
    m_WindowGuard->setIdleWindow(nullptr);
    for (auto& shell : m_Windows) shell->hide();
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
    s.m_QtInputCallbacks.publish(m_Input.get());
    bool ready = !m_Stop.load() && s.initialize(m_Factory.get()) && !m_Stop.load();
    if (ready) {
        m_Input->setStreamSize(QSize(s.m_StreamConfig.width, s.m_StreamConfig.height));
        ready = s.startConnectionAsync() && !m_Stop.load();
    }
    if (ready) {
        m_Input->startTablets(LiGetHostFeatureFlags());
        ready = resetDecoder();
    }
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
            m_Input->beginReconnect();
            Session::PlankReconnectState state;
            if (!s.beginPlankReconnect(state) || m_Stop.load()) break;
            m_Input->resetCursorEpoch();
            s.m_StreamConfig.bitrate = m_RecoveryBitrate.load();
            const bool connected = s.runPlankReconnect() && !m_Stop.load();
            // Use the established recovery state machine. It requests a
            // decoder reset only if format/size changed; otherwise retain it.
            bool needsReset = false;
            ready = s.finishPlankReconnect(connected, state, &needsReset) && !m_Stop.load();
            if (ready) m_Input->finishReconnect();
            if (ready && needsReset) ready = resetDecoder();
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
    QMetaObject::invokeMethod(this, [this] { m_Streaming = false; setInputEnabled(false); }, Qt::QueuedConnection);
    m_Input->shutdown();
    s.m_QtInputCallbacks.clear();
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
    setInputEnabled(false);
    m_WindowGuard->setIdleWindow(nullptr);
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
    m_Input->setStreamSize(size);
    m_Streaming = true;
    m_ReconnectNoticeDeadline = 0;
    updateGeometry();
    setStatus(m_Presenter->hasPresentedLiveFrame() ? QString() : tr("Connecting to workstation…"));
    for (auto& shell : m_Windows) {
        shell->surface().setWaiting(false);
        shell->surface().setEmbeddedCursor(!(LiGetHostFeatureFlags() & LI_FF_LOCAL_CURSOR));
    }
    setInputEnabled(m_InputGrabbed);
}

void QtSessionRuntime::updateGeometry()
{
    if (!m_Presenter || m_Windows.empty() || !m_StreamSize.isValid()) return;
    const bool fullscreen = m_Windows.front()->windowState() == Qt::WindowFullScreen;
    const bool multi = fullscreen && m_Windows.size() > 1;
    if (m_Windows.front()->windowState() == Qt::WindowMinimized) {
        for (size_t i = 1; i < m_Windows.size(); ++i) m_Windows[i]->hide();
        updateFocus();
        return;
    }
    QSize canvas(0, 0);
    QMap<QtSessionShell*, QRect> outputRects;
    if (multi) {
        std::vector<QtSessionShell*> ordered;
        for (const auto& shell : m_Windows) {
            if (!shell->screen()) return; // screenRemoved owns controlled teardown
            ordered.push_back(shell.get());
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->screen()->geometry().x() < b->screen()->geometry().x();
        });
        for (auto* shell : ordered) {
            const auto& surface = shell->surface();
            const QSize pixels(qRound(surface.width() * surface.devicePixelRatio()), qRound(surface.height() * surface.devicePixelRatio()));
            outputRects.insert(shell, QRect(QPoint(canvas.width(), 0), pixels));
            canvas.rwidth() += pixels.width();
            canvas.setHeight(qMax(canvas.height(), pixels.height()));
        }
    }
    for (size_t index = 0; index < m_Windows.size(); ++index) {
        auto& shell = *m_Windows[index];
        if (index && !multi) { shell.hide(); continue; }
        if (index && !shell.isVisible()) shell.showFullScreen();
        auto& surface = shell.surface();
        const QSize drawable(qRound(surface.width() * surface.devicePixelRatio()), qRound(surface.height() * surface.devicePixelRatio()));
        if (!drawable.isValid()) continue;
        QRect output(QPoint(), drawable);
        if (multi) output = outputRects.value(&shell);
        const auto slice = PlankPresentation::sliceForDrawable(m_StreamSize, multi ? canvas : drawable, output, drawable);
        const QtSessionGeometry geometry{m_StreamSize, slice.sourceRect, slice.destinationRect, drawable, surface.size()};
        surface.setPresentation(geometry);
        m_Presenter->setGeometry(&surface, geometry);
    }
    updateFocus();
}

void QtSessionRuntime::setStatus(const QString& text)
{
    for (auto& shell : m_Windows) shell->surface().setStatusText(text);
}

void QtSessionRuntime::beginRecovery()
{
    m_ReconnectNoticeDeadline = SDL_GetTicks() + quint64(m_Session->m_Preferences->plankUnreachableTimeoutSeconds) * 1000;
    if (m_Session->m_Preferences->plankUnreachableAction == StreamingPreferences::PLANK_UNREACHABLE_DISCONNECT)
        m_Session->m_ReconnectPolicy.allowUntil(m_ReconnectNoticeDeadline);
    else
        // The timeout reveals a status dialog; it does not suspend recovery.
        // The shared worker retains cancellation, bounded requests, retry delay,
        // terminal authentication errors and protection against silent takeover.
        m_Session->m_ReconnectPolicy.allowContinuously();
    for (auto& shell : m_Windows) shell->surface().setWaiting(false);
}

void QtSessionRuntime::setInputEnabled(bool enabled)
{
    for (auto& shell : m_Windows) shell->surface().setRemoteInputEnabled(enabled);
    m_Input->setEnabled(enabled);
    updateFocus();
}

void QtSessionRuntime::updateFocus()
{
    auto* focused = QGuiApplication::focusWindow();
    bool active = false;
    for (const auto& shell : m_Windows)
        active |= focused == shell.get() || focused == &shell->surface();
    m_Input->setFocused(active && !m_Stop.load());
    const auto policy = m_CapturePolicy;
    const bool fullscreen = !m_Windows.empty() && m_Windows.front()->windowState() == Qt::WindowFullScreen;
    const bool grab = active && m_Streaming && m_InputGrabbed && !m_Stop.load() &&
        (policy == StreamingPreferences::CSK_ALWAYS || (policy == StreamingPreferences::CSK_FULLSCREEN && fullscreen));
    m_Input->setSystemKeys(grab);
    QtSessionShell* focusedShell = nullptr;
    for (auto& shell : m_Windows)
        if (focused == shell.get() || focused == &shell->surface()) focusedShell = shell.get();
    m_WindowGuard->setWindows(focusedShell, focusedShell ? &focusedShell->surface() : nullptr);
    m_WindowGuard->setPolicy(grab, active && m_Streaming && m_InputGrabbed && m_ConfinePointer &&
        fullscreen && m_Windows.size() == 1);
    // The Wayland shortcut inhibitor is attached separately to Qt's surface.
    // XCB implements this public Qt API directly.
    if (QGuiApplication::platformName() != QStringLiteral("wayland"))
        for (auto& shell : m_Windows) shell->setKeyboardGrabEnabled(grab && shell.get() == focusedShell);
}

void QtSessionRuntime::toggleFullscreen()
{
    if (m_Windows.empty()) return;
    m_Fullscreen = !m_Fullscreen;
    for (size_t i = 0; i < m_Windows.size(); ++i) {
        m_Windows[i]->surface().cancelInput();
        if (m_Fullscreen) m_Windows[i]->showFullScreen();
        else if (!i) m_Windows[i]->showNormal();
        else m_Windows[i]->hide();
    }
    updateGeometry();
}

void QtSessionRuntime::minimize()
{
    for (auto& shell : m_Windows) shell->surface().cancelInput();
    if (!m_Windows.empty()) m_Windows.front()->showMinimized();
    for (size_t i = 1; i < m_Windows.size(); ++i) m_Windows[i]->hide();
}

void QtSessionRuntime::handleShortcut(int key)
{
    if (m_Windows.empty()) return;
    auto& surface = m_Windows.front()->surface();
    switch (key) {
    case Qt::Key_Q: stop(); break;
    case Qt::Key_Z: m_InputGrabbed = !m_InputGrabbed; setInputEnabled(m_Streaming && m_InputGrabbed); break;
    case Qt::Key_X: surface.toggleFullscreen(); break;
    case Qt::Key_S:
    {
        const bool show = !surface.statisticsVisible();
        for (auto& shell : m_Windows) shell->surface().setStatisticsVisible(show);
        break;
    }
    case Qt::Key_D: surface.minimize(); break;
    case Qt::Key_V: m_Input->pasteText(); break;
    case Qt::Key_K:
        m_CapturePolicy = m_CapturePolicy == StreamingPreferences::CSK_OFF ?
            StreamingPreferences::CSK_ALWAYS : StreamingPreferences::CSK_OFF;
        updateFocus(); break;
    case Qt::Key_L: m_ConfinePointer = !m_ConfinePointer; updateFocus(); break;
    default: break;
    }
}

void QtSessionRuntime::tick()
{
    if (m_Stop.load()) return;
    if (m_Session->m_ReconnectGreeterConfirmed.exchange(false)) setStatus(tr("Returning to the sign-in screen…"));
    if (m_ReconnectNoticeDeadline && SDL_GetTicks() >= m_ReconnectNoticeDeadline) {
        m_ReconnectNoticeDeadline = 0;
        if (m_Session->m_Preferences->plankUnreachableAction == StreamingPreferences::PLANK_UNREACHABLE_DISCONNECT) {
            stop();
            return;
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "PLANK Qt waiting for workstation; automatic recovery remains active");
            setInputEnabled(false);
            for (auto& shell : m_Windows) shell->surface().setWaiting(true);
        }
    }
    if (m_Streaming) {
        const bool silent = PlankHostRecovery::videoSilent(SDL_GetTicks(), m_Session->m_LastPlankVideoReceived.load());
        setStatus(silent ? tr("Waiting for workstation…") :
                  m_Presenter->hasPresentedLiveFrame() ? QString() : tr("Connecting to workstation…"));
    }
    const auto now = SDL_GetTicks();
    const auto requested = m_Session->m_ConfirmedBitrateRequestKbps.load();
    const auto applied = m_Session->m_ConfirmedBitrateAppliedKbps.load();
    if (requested == m_TargetBitrate && applied > 0 && applied != m_TargetBitrate) {
        m_TargetBitrate = applied; m_RecoveryBitrate.store(applied);
        for (auto& shell : m_Windows) shell->surface().setBitrateKbps(applied);
    }
    const auto flags = LI_FF_DYNAMIC_VIDEO_BITRATE | LI_FF_ENCODER_TARGET_ACK;
    const bool rateSupported = (LiGetHostFeatureFlags() & flags) == flags;
    if (m_Streaming && rateSupported && m_TargetBitrate != applied && now >= m_BitrateChanged + 250 &&
        now >= m_BitrateSent + (m_LastSentBitrate == m_TargetBitrate ? 1000 : 250)) {
        if (LiSetVideoBitrate(m_TargetBitrate) == 0) { m_LastSentBitrate = m_TargetBitrate; m_BitrateSent = now; }
    }
    QVariantMap telemetry{{QStringLiteral("fps"), m_Session->m_CurrentRenderedFps.load()},
        {QStringLiteral("videoMbps"), m_Session->m_CurrentVideoMbps.load()},
        {QStringLiteral("loss"), m_Session->currentVideoFecLoss().before},
        {QStringLiteral("rtt"), m_Session->currentNetworkRttMs()},
        {QStringLiteral("bitrateEnabled"), rateSupported && m_Streaming},
        {QStringLiteral("bitrateConfirmed"), requested == m_TargetBitrate && applied == m_TargetBitrate},
        {QStringLiteral("peakKbps"), m_Session->m_ConfirmedBitratePeakKbps.load()}};
    {
        std::unique_lock<std::mutex> lock(m_Session->m_MicrophoneMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            telemetry.insert(QStringLiteral("microphoneSupported"), m_Session->m_Microphone != nullptr);
            telemetry.insert(QStringLiteral("microphoneState"), int(m_Session->m_Microphone ? m_Session->m_Microphone->state() : PlankMicrophone::State::Off));
        }
    }
    {
        std::unique_lock<std::mutex> lock(m_Session->m_CameraMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            telemetry.insert(QStringLiteral("cameraSupported"), m_Session->m_Camera != nullptr);
            telemetry.insert(QStringLiteral("cameraState"), int(m_Session->m_Camera ? m_Session->m_Camera->state() : PlankCamera::State::Off));
        }
    }
    for (auto& shell : m_Windows) shell->surface().setTelemetry(telemetry);
}
