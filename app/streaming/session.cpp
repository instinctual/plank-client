#include <QtGlobal>
#include <QDebug>
#include <QThread>
#include "monotonicclock.h"
#include "session.h"
#include "streaming/clientframeflowtrace.h"
#include "backend/hostrecovery.h"
#include "backend/planknetwork.h"
#include "settings/streamingpreferences.h"
#include "streaming/avsynccontroller.h"
#include "streaming/plankdisplaymode.h"
#ifdef Q_OS_MACOS
#include "macclipboardsync.h"
#endif
#include "backend/computermanager.h"
#include "backend/nvaddress.h"
#ifdef Q_OS_MACOS
#include "macapplication.h"
#endif

#include <Limelight.h>
#include "utils.h"

#ifdef HAVE_FFMPEG
#include "video/ffmpeg.h"
#endif

#include <QtEndian>
#include <QCoreApplication>
#include <QThreadPool>
#include <QImage>
#include <QGuiApplication>
#include <QCursor>
#include <QWindow>
#include <QScreen>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#ifdef PLANK_TRANSPORT
#include "plank_transport.h"
#include "plank_transport_control.h"
#include "plank_transport_camera.h"
#include "plank_transport_event.h"
#include "plank_transport_input.h"
#include "plank_transport_setup.h"
#endif

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>
#include "qt/sessionruntime.h"
#include "qt/sessioninput.h"

CONNECTION_LISTENER_CALLBACKS Session::k_ConnCallbacks = {
    Session::clStageStarting,
    nullptr,
    Session::clStageFailed,
    nullptr,
    Session::clConnectionTerminated,
    Session::clLogMessage,
    nullptr,
    Session::clConnectionStatusUpdate,
    Session::clSetHdrMode,
    nullptr,
    nullptr,
    nullptr,
    Session::clRawHidControl,
    Session::clVideoBitrateApplied,
    Session::clCursorChunk,
    Session::clCursorPosition,
    nullptr, // Native transport publishes the paired FEC counters directly.
};

Session* Session::s_ActiveSession;
QSemaphore Session::s_ActiveSessionSemaphore(1);

void Session::clStageStarting(int stage)
{
    // We know this is called on the same thread as LiStartConnection()
    // which happens to be the main thread, so it's cool to interact
    // with the GUI in these callbacks.
    emit s_ActiveSession->stageStarting(QString::fromLocal8Bit(LiGetStageName(stage)));
}

void Session::clStageFailed(int stage, int errorCode)
{
    if (s_ActiveSession->m_Reconnecting.load()) {
        qWarning("PLANK reconnect stage failed: %s (%d)",
                    LiGetStageName(stage), errorCode);
        return;
    }

    QString failingEndpoint;
    if (stage == STAGE_SESSION_NEGOTIATION || stage == STAGE_CONTROL_STREAM_START) {
        failingEndpoint = QStringLiteral("UDP %1").arg(
                    s_ActiveSession->m_Computer->activeAddress.port());
    }

    emit s_ActiveSession->stageFailed(QString::fromLocal8Bit(LiGetStageName(stage)),
                                      errorCode,
                                      failingEndpoint);
}

void Session::clConnectionTerminated(int errorCode)
{
    if (static_cast<std::uint32_t>(errorCode) ==
            PLANK_TRANSPORT_TERMINATION_SESSION_TAKEN_OVER) {
        s_ActiveSession->m_CanReconnect.store(false);
        s_ActiveSession->m_ReconnectCancelled.store(true);
        s_ActiveSession->m_UnexpectedTermination = false;
        emit s_ActiveSession->displayLaunchError(
                    tr("This PLANK session was transferred to another client."));

        requestSessionQuit();
        return;
    }

    if (s_ActiveSession->m_Computer->plankAuthentication &&
            s_ActiveSession->m_CanReconnect.load() &&
            !s_ActiveSession->m_Reconnecting.load() &&
            !s_ActiveSession->m_ReconnectRequested.exchange(true)) {
        qWarning("PLANK transport ended (%d); starting responsive reconnect",
                    errorCode);
        emit s_ActiveSession->qtReconnectRequested();
        return;
    }

    // The early worker probe and transport termination can arrive together.
    // A queued reconnect is already owned by the Qt session worker, just like a
    // running one; duplicate callbacks must not enqueue a fatal quit.
    if (s_ActiveSession->m_ReconnectRequested.load() || s_ActiveSession->m_Reconnecting.load()) {
        qWarning("PLANK reconnect attempt ended: %d", errorCode);
        return;
    }

    // Display the termination dialog if this was not intended
    switch (errorCode) {
    case ML_ERROR_GRACEFUL_TERMINATION:
        break;

    case ML_ERROR_NO_VIDEO_TRAFFIC:
        s_ActiveSession->m_UnexpectedTermination = true;

        emit s_ActiveSession->displayLaunchError(tr("No video received from host.") + "\n\n"+
                                                 tr("Check your firewall rules for UDP port %1.")
                                                 .arg(s_ActiveSession->m_Computer->activeAddress.port()));
        break;

    case ML_ERROR_NO_VIDEO_FRAME:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Your network connection isn't performing well. Reduce your video bitrate setting or try a faster connection."));
        break;

    case ML_ERROR_PROTECTED_CONTENT:
    case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("Something went wrong on your host PC when starting the stream.") + "\n\n" +
                                                 tr("Make sure you don't have any DRM-protected content open on your host PC. You can also try restarting your host PC."));
        break;

    case ML_ERROR_FRAME_CONVERSION:
        s_ActiveSession->m_UnexpectedTermination = true;
        emit s_ActiveSession->displayLaunchError(tr("The host PC reported a fatal video encoding error.") + "\n\n" +
                                                 tr("Try disabling HDR mode, changing the streaming resolution, or changing your host PC's display resolution."));
        break;

    default:
        s_ActiveSession->m_UnexpectedTermination = true;

        // We'll assume large errors are hex values
        bool hexError = qAbs(errorCode) > 1000;
        emit s_ActiveSession->displayLaunchError(tr("Connection terminated") + "\n\n" +
                                                 tr("Error code: %1").arg(errorCode, hexError ? 8 : 0, hexError ? 16 : 10, QChar('0')));
        break;
    }

    qCritical("Connection terminated: %d",
                 errorCode);

    // Push a quit event to the main loop
    requestSessionQuit();
}

void Session::requestSessionQuit()
{
    if (s_ActiveSession) emit s_ActiveSession->qtQuitRequested();
}

void Session::requestDecoderReset()
{
    if (s_ActiveSession) emit s_ActiveSession->qtDecoderResetRequested();
}

void Session::clLogMessage(const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    const auto message = QString::vasprintf(format, ap);
    va_end(ap);
    qInfo().noquote() << message;
}

void Session::clConnectionStatusUpdate(int connectionStatus)
{
    if (s_ActiveSession->m_Reconnecting.load() || !s_ActiveSession->m_Preferences->connectionWarnings) return;
    if (connectionStatus == CONN_STATUS_POOR) {
        s_ActiveSession->setPlankReconnectStatus(s_ActiveSession->m_StreamConfig.bitrate > 5000 ?
            "Slow connection to PC\nReduce your bitrate" : "Poor connection to PC", true);
    } else if (connectionStatus == CONN_STATUS_OKAY) {
        s_ActiveSession->setPlankReconnectStatus("", false);
    }
}

void Session::clSetHdrMode(bool enabled)
{
    // If we're in the process of recreating our decoder when we get
    // this callback, we'll drop it. The main thread will make the
    // callback when it finishes creating the new decoder.
    if (std::unique_lock<std::mutex> lock(s_ActiveSession->m_DecoderLock, std::try_to_lock); lock) {
        IVideoDecoder* decoder = s_ActiveSession->m_VideoDecoder;
        if (decoder != nullptr) {
            decoder->setHdrMode(enabled);
        }
    }
}

void Session::clRawHidControl(const unsigned char* data, unsigned int length)
{
    if (s_ActiveSession) {
        s_ActiveSession->m_QtInputCallbacks.invoke([&](QtSessionInput& input) { input.handleRawHidControl(data, length); });
    }
}

void Session::clVideoBitrateApplied(
        uint32_t requestedKbps, uint32_t appliedKbps, uint32_t peakKbps)
{
    if (s_ActiveSession == nullptr) {
        return;
    }

    s_ActiveSession->m_ConfirmedBitrateRequestKbps.store(
                static_cast<int>(requestedKbps), std::memory_order_relaxed);
    s_ActiveSession->m_ConfirmedBitrateAppliedKbps.store(
                static_cast<int>(appliedKbps), std::memory_order_relaxed);
    s_ActiveSession->m_ConfirmedBitratePeakKbps.store(
                static_cast<int>(peakKbps), std::memory_order_relaxed);

}

void Session::clCursorChunk(const unsigned char* data, unsigned int length)
{
    if (s_ActiveSession) {
        s_ActiveSession->m_QtInputCallbacks.invoke([&](QtSessionInput& input) { input.handleCursorShape(data, length); });
    }
}

void Session::clCursorPosition(const unsigned char* data, unsigned int length)
{
    if (s_ActiveSession) {
        s_ActiveSession->m_QtInputCallbacks.invoke([&](QtSessionInput& input) { input.handleCursorPosition(data, length); });
    }
}

void Session::updateVideoFecLoss(VideoFecLossPercent loss)
{
    Session* session = s_ActiveSession;
    if (session == nullptr) {
        return;
    }

    const quint64 now = PlankClock::milliseconds();
    std::lock_guard<std::mutex> lock(session->m_VideoPacketLossSamplesLock);
    // Publish both ten-second peaks together. Toolbar and overlay share this
    // snapshot; frame drops and decoder/render queues are separate metrics.
    session->m_CurrentVideoFecLoss = {
        session->m_VideoPacketLossPeakWindow.addSample(now, loss.before),
        session->m_VideoPacketLossAfterFecPeakWindow.addSample(now, loss.after)
    };
}

bool Session::chooseDecoder(DecoderSelectionMode selectionMode,
                            int videoFormat, int width, int height,
                            int frameRate, bool enableVsync, bool testOnly,
                            IVideoDecoder*& chosenDecoder, bool enableIdentityGbr,
                            DecoderCaptureSource captureSource,
                            DecoderEncoderBackend encoderBackend,
                            IFFmpegRendererFactory* presenter)
{
    DECODER_PARAMETERS params = {};

    // We should never have vsync enabled for test-mode.
    // It introduces unnecessary delay for renderers that may
    // block while waiting for a backbuffer swap.
    Q_ASSERT(!enableVsync || !testOnly);

    params.width = width;
    params.height = height;
    params.frameRate = frameRate;
    params.videoFormat = videoFormat;
    params.enableVsync = enableVsync;
    params.enableIdentityGbr = enableIdentityGbr;
    params.testOnly = testOnly;
    params.selectionMode = selectionMode;
    params.captureSource = captureSource;
    params.encoderBackend = encoderBackend;
    params.rendererFactory = presenter;

    qInfo("V-sync %s",
                enableVsync ? "enabled" : "disabled");

#ifdef HAVE_FFMPEG
    chosenDecoder = new FFmpegVideoDecoder(testOnly);
    if (chosenDecoder->initialize(&params)) {
        qInfo("FFmpeg-based video decoder chosen");
        return true;
    }
    else {
        qCritical("Unable to load FFmpeg decoder");
        delete chosenDecoder;
        chosenDecoder = nullptr;
    }
#endif

#ifndef HAVE_FFMPEG
#error No video decoding libraries available!
#endif

    // If we reach this, we didn't initialize any decoders successfully
    return false;
}

bool Session::isIdentityGbrEnabledForFormat(int videoFormat) const
{
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) return false;
    return (videoFormat == VIDEO_FORMAT_H264_HIGH8_444 ||
            videoFormat == VIDEO_FORMAT_H264_HIGH10_444 ||
            videoFormat == VIDEO_FORMAT_H265_REXT8_444 ||
            videoFormat == VIDEO_FORMAT_H265_REXT10_444) &&
           (m_Computer->serverCodecModeSupport & SCM_IDENTITY_GBR_444);
}

int Session::drSetup(int videoFormat, int width, int height, int frameRate, void *, int)
{
    s_ActiveSession->m_ActiveVideoFormat = videoFormat;
    s_ActiveSession->m_ActiveVideoWidth = width;
    s_ActiveSession->m_ActiveVideoHeight = height;
    s_ActiveSession->m_ActiveVideoFrameRate = frameRate;

    // The Qt session worker creates the decoder after stream negotiation.

    qInfo("Video stream is %dx%dx%d (format 0x%x)",
                width, height, frameRate, videoFormat);
    if (s_ActiveSession->isIdentityGbrEnabledForFormat(videoFormat)) {
        if (videoFormat == VIDEO_FORMAT_H264_HIGH8_444) {
            qInfo("Video precision: native 8-bit RGB -> "
                        "8-bit H.264 4:4:4 -> 8-bit RGB identity presentation");
        }
        else if (videoFormat == VIDEO_FORMAT_H265_REXT8_444) {
            qInfo("Video precision: native 8-bit NvFBC source -> "
                        "8-bit HEVC 4:4:4 -> 8-bit RGB identity presentation");
        }
        else {
            const bool native10 = s_ActiveSession->m_PlankCaptureSource ==
                    StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10;
            qInfo("Video precision: %s -> 10-bit %s 4:4:4 -> "
                        "10-bit RGB identity presentation",
                        native10 ? "native 10-bit X11/XShm" :
                                   "8-bit NvFBC source/up-converted",
                        videoFormat == VIDEO_FORMAT_H264_HIGH10_444 ? "H.264" : "HEVC");
        }
    }
    else if (videoFormat == VIDEO_FORMAT_H264_HIGH8_422 ||
             videoFormat == VIDEO_FORMAT_H264_HIGH10_422) {
        qInfo("Video precision: 8-bit source -> %s H.264 4:2:2 -> "
                    "BT.709 full-range YCbCr presentation",
                    videoFormat == VIDEO_FORMAT_H264_HIGH10_422 ? "10-bit" : "8-bit");
    }

    return 0;
}

int Session::drSubmitDecodeUnit(PDECODE_UNIT du)
{
    // Reconnect/teardown owns the decoder while this lock is unavailable.
    std::unique_lock<std::mutex> lock(s_ActiveSession->m_DecoderLock, std::try_to_lock);
    if (!lock || !s_ActiveSession->m_VideoDecoder) return DR_OK;
    return s_ActiveSession->m_VideoDecoder->submitDecodeUnit(du);
}

Session::DecoderAvailability
Session::getDecoderAvailability(                                int videoFormat, int width, int height, int frameRate,
                                bool enableIdentityGbr)
{
    IVideoDecoder* decoder;

    if (!chooseDecoder(DecoderSelectionMode::PreferExactHardwareThenSoftware,
                       videoFormat, width, height, frameRate,
                       false, true, decoder, enableIdentityGbr,
                       decoderCaptureSource(), decoderEncoderBackend(), m_RendererFactory)) {
        return DecoderAvailability::None;
    }

    bool hw = decoder->isHardwareAccelerated();

    delete decoder;

    return hw ? DecoderAvailability::Hardware : DecoderAvailability::Software;
}

DecoderCaptureSource Session::decoderCaptureSource() const
{
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
        return DecoderCaptureSource::ScreenCaptureKit;
    }
    return m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10 ?
                DecoderCaptureSource::NativeX11_10Bit : DecoderCaptureSource::Nvfbc8Bit;
}

DecoderEncoderBackend Session::decoderEncoderBackend() const
{
    if (StreamingPreferences::isPlankAppleProfile(m_PlankVideoProfile)) {
        return DecoderEncoderBackend::VideoToolbox;
    }
    return StreamingPreferences::isPlankNvencProfile(m_PlankVideoProfile) ?
                DecoderEncoderBackend::NvencDirect : DecoderEncoderBackend::SoftwareCuda;
}

bool Session::populateDecoderProperties()
{
    IVideoDecoder* decoder;

    if (!chooseDecoder(DecoderSelectionMode::PreferExactHardwareThenSoftware,
                       m_SupportedVideoFormats.first(),
                       m_StreamConfig.width,
                       m_StreamConfig.height,
                       m_StreamConfig.fps,
                       false, true, decoder,
                       isIdentityGbrEnabledForFormat(m_SupportedVideoFormats.first()),
                       decoderCaptureSource(), decoderEncoderBackend(), m_RendererFactory)) {
        return false;
    }

    m_VideoCallbacks.capabilities = decoder->getDecoderCapabilities();
    if (m_VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) {
        // It is an error to pass a push callback when in pull mode
        m_VideoCallbacks.submitDecodeUnit = nullptr;
    }
    else {
        m_VideoCallbacks.submitDecodeUnit = drSubmitDecodeUnit;
    }

    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
        // This profile has an exact, negotiated color contract. An environment
        // override must not reinterpret its YCbCr samples as full-range or RGB.
        m_StreamConfig.colorSpace = COLORSPACE_REC_709;
        m_StreamConfig.colorRange = COLOR_RANGE_LIMITED;
    }
    else {
        bool ok;

        m_StreamConfig.colorSpace = qEnvironmentVariableIntValue("COLOR_SPACE_OVERRIDE", &ok);
        if (ok) {
            qWarning("Using colorspace override: %d",
                        m_StreamConfig.colorSpace);
        }
        else {
            m_StreamConfig.colorSpace = decoder->getDecoderColorspace();
        }

        m_StreamConfig.colorRange = qEnvironmentVariableIntValue("COLOR_RANGE_OVERRIDE", &ok);
        if (ok) {
            qWarning("Using color range override: %d",
                        m_StreamConfig.colorRange);
        }
        else {
            m_StreamConfig.colorRange = decoder->getDecoderColorRange();
        }
    }

    if (decoder->isAlwaysFullScreen()) {
        m_IsFullScreen = true;
    }

    delete decoder;

    return true;
}

Session::Session(NvComputer* computer, NvApp& app,
                 StreamingPreferences *preferences,
                 ComputerManager *computerManager)
    : m_Preferences(preferences ? preferences : StreamingPreferences::get()),
      m_IsFullScreen(m_Preferences->windowMode != StreamingPreferences::WM_WINDOWED || !WMUtils::isRunningDesktopEnvironment()),
      m_Computer(computer),
      m_PlankVideoProfile(static_cast<StreamingPreferences::PlankVideoProfile>(
              qBound(static_cast<int>(StreamingPreferences::PLANK_PROFILE_H264_10BIT_444),
                     computer->plankVideoProfile,
                     static_cast<int>(StreamingPreferences::PLANK_PROFILE_COUNT) - 1))),
      m_PlankCaptureSource(static_cast<StreamingPreferences::PlankCaptureSource>(
              qBound(static_cast<int>(StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT),
                     computer->plankCaptureSource,
                     static_cast<int>(StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT)))),
      m_PlankBitrateKbps(
              StreamingPreferences::plankBitrateForProfile(
                  computer->plankProfileBitratesKbps,
                  computer->plankVideoProfile)),
      m_ComputerManager(computerManager),
      m_App(app),
      m_VideoDecoder(nullptr),
      m_AudioMuted(false),
      m_UnexpectedTermination(true), // Failure prior to streaming is unexpected
      m_ReconnectRequested(false),
      m_Reconnecting(false),
      m_ReconnectCancelled(false),
      m_CanReconnect(false),
      m_ConnectionStartCancelled(false),
      m_WaitingForSessionCleanup(false),
      m_AsyncConnectionSuccess(false),
      m_OpusDecoder(nullptr),
      m_AudioRenderer(nullptr),
      m_AudioSampleCount(0),
      m_DropAudioEndTime(0),
      m_AudioMediaFramesReceived(0),
      m_AvSyncTelemetryEnabled(qEnvironmentVariableIntValue("PLANK_AV_SYNC_TELEMETRY") > 0),
      m_LastAudioTelemetryTime(0),
      m_CurrentRenderedFps(0.0f),
      m_CurrentVideoMbps(0.0f),
      m_CurrentNetworkRttMs(0)
{
    if (m_Computer->plankAuthentication) {
        if (m_ComputerManager != nullptr) {
            m_ComputerManager->takePlankReconnectCredentials(
                        m_Computer, m_PlankUsername,
                        m_PlankPassword);
            m_CanReconnect.store(!m_PlankUsername.isEmpty() &&
                                 !m_PlankPassword.isEmpty());
        }
        PlankAvSync::resetVideoClock();

        // PLANK is a qualified workstation protocol, not a generic
        // game-streaming profile. Its stream size is selected from the target
        // client displays or the bookmark's explicit layout.
        // The bookmark owns one startup encoder target for each exact encoding
        // profile. The selected value is a session-local copy, and the toolbar
        // never writes changes back to the bookmark.
        m_Preferences->fps = 60;
        m_Preferences->identityGbrBitDepth =
                (m_PlankVideoProfile ==
                     StreamingPreferences::PLANK_PROFILE_H264_8BIT_422 ||
                 m_PlankVideoProfile ==
                     StreamingPreferences::PLANK_PROFILE_H264_8BIT_444 ||
                 m_PlankVideoProfile ==
                     StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444 ||
                 m_PlankVideoProfile ==
                     StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444) ? 8 : 10;
        // Decoder selection is internal and exact-profile constrained. Hardware
        // is accepted only after a test frame proves the requested bit depth,
        // chroma sampling, and identity mapping; otherwise the same profile
        // falls back to FFmpeg software decoding without changing formats.
    }
}

Session::~Session()
{
    stopPlankTransportDataPlane();
    clearPlankReconnectCredentials();
}

bool Session::startPlankTransportDataPlane(quint16 port,
                                      const QString& certificateSha256,
                                      const QString& token,
                                      quint16 quicUdpPayloadMtu)
{
#ifndef PLANK_TRANSPORT
    Q_UNUSED(port)
    Q_UNUSED(certificateSha256)
    Q_UNUSED(token)
    Q_UNUSED(quicUdpPayloadMtu)
    return false;
#else
    stopPlankTransportDataPlane();

    QHostAddress remoteHost(m_Computer->activeAddress.address());
    if (remoteHost.isNull()) {
        const QHostInfo resolved = QHostInfo::fromName(
                    m_Computer->activeAddress.address());
        for (const QHostAddress& candidate : resolved.addresses()) {
            if (candidate.protocol() == QAbstractSocket::IPv4Protocol) {
                remoteHost = candidate;
                break;
            }
        }
        if (remoteHost.isNull() && !resolved.addresses().isEmpty()) {
            remoteHost = resolved.addresses().first();
        }
    }
    if (remoteHost.isNull()) {
        qWarning() << "Unable to resolve the PLANK transport endpoint";
        return false;
    }

    const QString remoteAddress = remoteHost.protocol() == QAbstractSocket::IPv6Protocol ?
                QStringLiteral("[%1]:%2").arg(remoteHost.toString()).arg(port) :
                QStringLiteral("%1:%2").arg(remoteHost.toString()).arg(port);
    const QByteArray remoteAddressUtf8 = remoteAddress.toUtf8();
    const QByteArray serverNameUtf8("plank");
    const QByteArray certificateUtf8 = certificateSha256.toLatin1();
    const QByteArray tokenUtf8 = token.toLatin1();

    PlankTransportConfig config {};
    config.struct_size = sizeof(config);
    config.abi_version = PLANK_TRANSPORT_ABI_VERSION;
    config.mode = PLANK_TRANSPORT_MODE_CLIENT;
    config.handshake_timeout_ms = 10000;
    config.idle_timeout_ms = 30000;
    config.keep_alive_interval_ms = 5000;
    config.max_udp_payload_size = quicUdpPayloadMtu;
    qInfo() << "Using the negotiated fixed maximum QUIC UDP payload:"
            << config.max_udp_payload_size << "bytes";
    config.remote_address = remoteAddressUtf8.constData();
    config.server_name = serverNameUtf8.constData();
    config.certificate_sha256 = certificateUtf8.constData();
    config.session_token = tokenUtf8.constData();

    PlankTransportNativeEndpoint* endpoint = nullptr;
    int result = plank_transport_native_endpoint_create(&config, &endpoint);
    if (result == PLANK_TRANSPORT_OK) {
        result = plank_transport_native_endpoint_start(endpoint);
    }
    if (result == PLANK_TRANSPORT_OK) {
        const auto deadline = PlankClock::milliseconds() + 12000;
        do {
            if (m_ConnectionStartCancelled.load()) { result = PLANK_TRANSPORT_ERROR_INVALID_STATE; break; }
            result = plank_transport_native_endpoint_wait_ready(endpoint, 100);
        } while (result == PLANK_TRANSPORT_TIMEOUT && PlankClock::milliseconds() < deadline);
    }
    if (result == PLANK_TRANSPORT_OK && m_MicrophoneNegotiated) {
        result = plank_transport_native_microphone_enable_version(endpoint, m_MicrophoneSchema);
    }
    if (result == PLANK_TRANSPORT_OK && m_CameraNegotiated &&
            plank_transport_native_camera_enable(endpoint, m_MicrophoneNegotiated ? 1 : 0) != PLANK_TRANSPORT_OK) {
        qWarning() << "PLANK camera lane is unavailable; other session media remain available";
    }
    if (result != PLANK_TRANSPORT_OK) {
        QByteArray error(512, '\0');
        if (endpoint != nullptr) {
            plank_transport_native_endpoint_last_error(
                        endpoint, error.data(), static_cast<size_t>(error.size()));
            plank_transport_native_endpoint_stop(endpoint);
            plank_transport_native_endpoint_destroy(endpoint);
        }
        qWarning() << "Experimental plank_transport handshake failed:" << error.constData();
        return false;
    }

    m_PlankTransportEndpoint = endpoint;
    LiSetPlankNativeControlSender(
                &Session::plankTransportNativeControlSender, endpoint);
    LiSetPlankNativeInputSender(
                &Session::plankTransportNativeInputSender, endpoint);
    qInfo() << "Experimental native KyProto media, input, and data protocols are ready on UDP"
            << port;
    return true;
#endif
}

bool Session::negotiatePlankTransportSession(quint16 sessionPort, QString& errorMessage)
{
#ifndef PLANK_TRANSPORT
    Q_UNUSED(sessionPort)
    errorMessage = tr("Native PLANK session negotiation is unavailable.");
    return false;
#else
    if (m_PlankTransportEndpoint == nullptr) {
        errorMessage = tr("The native PLANK transport is not connected.");
        return false;
    }

    const int negotiatedVideoFormat = m_StreamConfig.supportedVideoFormats;
    int codec = 0;
    int chroma = 1;
    bool tenBit = false;
    switch (negotiatedVideoFormat) {
    case VIDEO_FORMAT_H264_HIGH8_422:
        chroma = 2;
        break;
    case VIDEO_FORMAT_H264_HIGH8_444:
        break;
    case VIDEO_FORMAT_H264_HIGH10_422:
        chroma = 2;
        tenBit = true;
        break;
    case VIDEO_FORMAT_H264_HIGH10_444:
        tenBit = true;
        break;
    case VIDEO_FORMAT_H265_REXT8_444:
        codec = 1;
        break;
    case VIDEO_FORMAT_H265_REXT10_444:
        codec = 1;
        tenBit = true;
        break;
    case VIDEO_FORMAT_H265_MAIN10:
        codec = 1;
        chroma = 0;
        tenBit = true;
        break;
    default:
        errorMessage = tr("The selected PLANK video profile has no native negotiation mapping.");
        return false;
    }

    int slicesPerFrame = (m_VideoCallbacks.capabilities >> 24) & 0xFF;
    if (slicesPerFrame == 0) {
        slicesPerFrame = 1;
    }
    const bool supportsReferenceFrameInvalidation = codec == 0 ?
                (m_VideoCallbacks.capabilities &
                 CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC) != 0 :
                (m_VideoCallbacks.capabilities &
                 CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC) != 0;
    const int referenceFrames = supportsReferenceFrameInvalidation ? 0 : 1;
    const int audioChannels = CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(
                m_StreamConfig.audioConfiguration);
    const int audioChannelMask = CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(
                m_StreamConfig.audioConfiguration);
    const bool highQualityAudio =
            m_StreamConfig.bitrate >= 15000 &&
            (m_AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) == 0 &&
            (audioChannels <= 2 ||
             !NvComputer::isVpnReachability(
                 m_Computer->getActiveAddressReachability()));
    const int packetDurationMs =
            (m_AudioCallbacks.capabilities & CAPABILITY_SLOW_OPUS_DECODER) != 0 ?
                10 : 5;

    const QJsonObject request {
        {QStringLiteral("video"), QJsonObject {
            {QStringLiteral("width"), m_StreamConfig.width},
            {QStringLiteral("height"), m_StreamConfig.height},
            {QStringLiteral("fps"), m_StreamConfig.fps},
            {QStringLiteral("fps_x100"), m_StreamConfig.clientRefreshRateX100},
            {QStringLiteral("slices_per_frame"), slicesPerFrame},
            {QStringLiteral("reference_frames"), referenceFrames},
            {QStringLiteral("encoder_csc_mode"),
             (m_StreamConfig.colorSpace << 1) | m_StreamConfig.colorRange},
            {QStringLiteral("codec"), codec},
            {QStringLiteral("ten_bit"), tenBit},
            {QStringLiteral("chroma"), chroma},
            {QStringLiteral("intra_refresh"), 0},
            {QStringLiteral("encoder_target_kbps"), m_StreamConfig.bitrate},
            {QStringLiteral("negotiated_format"), negotiatedVideoFormat},
        }},
        {QStringLiteral("audio"), QJsonObject {
            {QStringLiteral("channels"), audioChannels},
            {QStringLiteral("channel_mask"), audioChannelMask},
            {QStringLiteral("packet_duration_ms"), packetDurationMs},
            {QStringLiteral("high_quality"), highQualityAudio},
        }},
    };
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    std::vector<unsigned char> packet(
                PLANK_TRANSPORT_SETUP_HEADER_SIZE + static_cast<size_t>(payload.size()));
    size_t packetSize = 0;
    constexpr uint32_t RequestId = 1;
    if (plank_transport_setup_encode(
                PLANK_TRANSPORT_SETUP_LAUNCH_REQUEST, 0,
                PLANK_TRANSPORT_SETUP_STATUS_OK, RequestId,
                reinterpret_cast<const uint8_t*>(payload.constData()),
                static_cast<size_t>(payload.size()),
                packet.data(), packet.size(), &packetSize) != 0 ||
            plank_transport_native_data_send(
                m_PlankTransportEndpoint, packet.data(), packetSize) !=
                PLANK_TRANSPORT_OK) {
        errorMessage = tr("The native PLANK launch request could not be sent.");
        return false;
    }

    packet.resize(PLANK_TRANSPORT_SETUP_MAX_PACKET_SIZE);
    packetSize = 0;
    const auto deadline = PlankClock::milliseconds() + 12000;
    int receiveResult;
    do {
        if (m_ConnectionStartCancelled.load()) { receiveResult = PLANK_TRANSPORT_ERROR_INVALID_STATE; break; }
        receiveResult = plank_transport_native_data_receive(
                    m_PlankTransportEndpoint, packet.data(), packet.size(), &packetSize, 100);
    } while (receiveResult == PLANK_TRANSPORT_TIMEOUT && PlankClock::milliseconds() < deadline);
    if (receiveResult != PLANK_TRANSPORT_OK) {
        errorMessage = tr("The host did not complete native session negotiation.");
        return false;
    }

    PlankTransportSetupPacket responsePacket {};
    if (plank_transport_setup_decode(packet.data(), packetSize, &responsePacket) != 0 ||
            responsePacket.request_id != RequestId ||
            (responsePacket.flags & PLANK_TRANSPORT_SETUP_FLAG_RESPONSE) == 0 ||
            (responsePacket.type != PLANK_TRANSPORT_SETUP_LAUNCH_RESPONSE &&
             responsePacket.type != PLANK_TRANSPORT_SETUP_ERROR)) {
        errorMessage = tr("The host returned an invalid native session response.");
        return false;
    }

    QJsonParseError parseError {};
    const QJsonDocument responseDocument = QJsonDocument::fromJson(
                QByteArray(reinterpret_cast<const char*>(responsePacket.payload),
                           static_cast<int>(responsePacket.payload_size)),
                &parseError);
    if (parseError.error != QJsonParseError::NoError ||
            !responseDocument.isObject()) {
        errorMessage = tr("The host returned malformed native session values.");
        return false;
    }
    const QJsonObject response = responseDocument.object();
    if (responsePacket.status != PLANK_TRANSPORT_SETUP_STATUS_OK ||
            responsePacket.type == PLANK_TRANSPORT_SETUP_ERROR) {
        const QString hostMessage = response.value(QStringLiteral("message")).toString();
        errorMessage = hostMessage.isEmpty() ?
                    tr("The host rejected native session negotiation.") : hostMessage;
        return false;
    }

    const QJsonObject audio = response.value(QStringLiteral("audio")).toObject();
    const QJsonArray mapping = audio.value(QStringLiteral("mapping")).toArray();
    const int responseVideoFormat = response.value(
                QStringLiteral("video_format")).toInt();
    const int sampleRate = audio.value(QStringLiteral("sample_rate")).toInt();
    const int responseChannels = audio.value(QStringLiteral("channels")).toInt();
    const int streams = audio.value(QStringLiteral("streams")).toInt();
    const int coupledStreams = audio.value(QStringLiteral("coupled_streams")).toInt();
    const int responsePacketDuration = audio.value(
                QStringLiteral("packet_duration_ms")).toInt();
    const int hostFeatureFlags = response.value(
                QStringLiteral("host_feature_flags")).toInt();
    const int referenceFrameInvalidation = response.value(
                QStringLiteral("reference_frame_invalidation")).toInt(-1);
    if (responseVideoFormat != negotiatedVideoFormat || sampleRate != 48000 ||
            responseChannels != audioChannels || responseChannels <= 0 ||
            responseChannels > AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT ||
            streams <= 0 || streams > responseChannels ||
            coupledStreams < 0 || coupledStreams > streams ||
            responsePacketDuration <= 0 || responsePacketDuration > 120 ||
            (hostFeatureFlags & LI_FF_LOCAL_CURSOR) == 0 ||
            (referenceFrameInvalidation != 0 && referenceFrameInvalidation != 1) ||
            mapping.size() != responseChannels) {
        errorMessage = tr("The host returned unsupported native audio or video values.");
        return false;
    }

    PLANK_NATIVE_SESSION_CONFIGURATION nativeConfiguration {};
    nativeConfiguration.structSize = sizeof(nativeConfiguration);
    nativeConfiguration.serviceFlags = PLANK_NATIVE_SERVICE_AUDIO |
            PLANK_NATIVE_SERVICE_INPUT | PLANK_NATIVE_SERVICE_LOCAL_CURSOR;
    nativeConfiguration.negotiatedVideoFormat = responseVideoFormat;
    nativeConfiguration.hostFeatureFlags =
            static_cast<unsigned int>(hostFeatureFlags);
    nativeConfiguration.audioPacketDurationMs = responsePacketDuration;
    nativeConfiguration.referenceFrameInvalidationSupported =
            static_cast<unsigned int>(referenceFrameInvalidation);
    nativeConfiguration.sessionPort = sessionPort;
    nativeConfiguration.opusConfiguration.sampleRate = sampleRate;
    nativeConfiguration.opusConfiguration.channelCount = responseChannels;
    nativeConfiguration.opusConfiguration.streams = streams;
    nativeConfiguration.opusConfiguration.coupledStreams = coupledStreams;
    for (int index = 0; index < responseChannels; ++index) {
        const int channel = mapping.at(index).toInt(-1);
        if (channel < 0 || channel >= responseChannels) {
            errorMessage = tr("The host returned an invalid native audio channel map.");
            return false;
        }
        nativeConfiguration.opusConfiguration.mapping[index] =
                static_cast<unsigned char>(channel);
    }
    if (LiSetPlankNativeSessionConfiguration(&nativeConfiguration) != 0) {
        errorMessage = tr("The native PLANK session values were rejected locally.");
        return false;
    }

    qInfo() << "Native QUIC session negotiated: video format"
            << QString::number(responseVideoFormat, 16)
            << "host features" << QString::number(hostFeatureFlags, 16)
            << "audio channels" << responseChannels
            << "packet duration" << responsePacketDuration << "ms";
    return true;
#endif
}

void Session::stopPlankTransportDataPlane()
{
#ifdef PLANK_TRANSPORT
    if (m_PlankTransportEndpoint != nullptr) {
        unsigned char packet[PLANK_TRANSPORT_CONTROL_MAX_PACKET_SIZE];
        size_t packetSize = 0;
        if (plank_transport_control_encode(
                    PLANK_TRANSPORT_CONTROL_CLIENT_DISCONNECT, nullptr, 0,
                    packet, sizeof(packet), &packetSize) == 0) {
            plank_transport_native_data_send(m_PlankTransportEndpoint, packet, packetSize);
        }
    }
    stopPlankTransportMediaReceivers();
    {
        std::lock_guard<std::mutex> guard(m_MicrophoneMutex);
        m_Microphone.reset();
    }
    {
        std::lock_guard<std::mutex> guard(m_CameraMutex);
        m_Camera.reset(); m_CameraRequested.store(false);
    }
    LiSetPlankNativeControlSender(nullptr, nullptr);
    LiSetPlankNativeInputSender(nullptr, nullptr);
    if (m_PlankTransportEndpoint != nullptr) {
        PlankTransportNativeStats stats {};
        stats.struct_size = sizeof(stats);
        if (plank_transport_native_endpoint_stats(m_PlankTransportEndpoint, &stats) ==
                PLANK_TRANSPORT_OK) {
            qInfo() << "PlankTransport native transport: video-frames="
                    << stats.video_frames_received
                    << "video-bytes=" << stats.video_bytes_received
                    << "video-receive-drops=" << stats.video_receive_drops
                    << "audio-packets=" << stats.audio_packets_received
                    << "audio-bytes=" << stats.audio_bytes_received
                    << "audio-receive-drops=" << stats.audio_receive_drops
                    << "input-sent=" << stats.input_packets_sent
                    << "data-sent=" << stats.data_packets_sent
                    << "data-received=" << stats.data_packets_received
                    << "QUIC-lost=" << stats.quic_packets_lost
                    << "QUIC-RTT-us=" << stats.quic_rtt_us
                    << "KyProto-drops=" << stats.kyproto_packets_dropped
                    << "video-FEC-source-symbols="
                    << stats.video_fec_source_symbols
                    << "video-FEC-source-symbols-missing="
                    << stats.video_fec_source_symbols_missing
                    << "video-FEC-source-symbols-unrecovered="
                    << stats.video_fec_source_symbols_unrecovered;
        }
        QByteArray lastError(512, '\0');
        plank_transport_native_endpoint_last_error(
                    m_PlankTransportEndpoint, lastError.data(),
                    static_cast<size_t>(lastError.size()));
        if (!lastError.isEmpty() && lastError.constData()[0] != '\0') {
            qWarning() << "PlankTransport native endpoint ended:"
                       << lastError.constData();
        }
        plank_transport_native_endpoint_stop(m_PlankTransportEndpoint);
        plank_transport_native_endpoint_destroy(m_PlankTransportEndpoint);
        m_PlankTransportEndpoint = nullptr;
        qInfo() << "Experimental native KyProto connection stopped";
    }
#endif
}

#ifdef PLANK_TRANSPORT
void Session::startPlankTransportMediaReceivers()
{
    stopPlankTransportMediaReceivers();
    if (m_PlankTransportEndpoint == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_VideoPacketLossSamplesLock);
        m_VideoPacketLossPeakWindow.reset();
        m_VideoPacketLossAfterFecPeakWindow.reset();
        m_CurrentVideoFecLoss = {};
    }
    m_CurrentNetworkRttMs.store(0, std::memory_order_relaxed);
    m_LastPlankVideoReceived.store(0);
    m_PlankTransportReceiversStopping.store(false);
#ifdef Q_OS_MACOS
    startClipboardSync();
#endif
    m_PlankTransportVideoThread = std::thread([this]() {
        plankTransportVideoReceiveLoop();
    });
    if (LiGetPlankNativeServiceFlags() & PLANK_NATIVE_SERVICE_AUDIO) {
        m_PlankTransportAudioThread = std::thread([this]() {
            plankTransportAudioReceiveLoop();
        });
    }
    m_PlankTransportDataThread = std::thread([this]() {
        plankTransportDataReceiveLoop();
    });
}

void Session::stopPlankTransportMediaReceivers()
{
#ifdef Q_OS_MACOS
#endif
    m_PlankTransportReceiversStopping.store(true);
    if (m_PlankTransportVideoThread.joinable()) {
        m_PlankTransportVideoThread.join();
    }
    if (m_PlankTransportAudioThread.joinable()) {
        m_PlankTransportAudioThread.join();
    }
    if (m_PlankTransportDataThread.joinable()) {
        m_PlankTransportDataThread.join();
    }
#ifdef Q_OS_MACOS
    stopClipboardSync();
#endif
}

void Session::plankTransportVideoReceiveLoop()
{
    ClientFrameFlowTrace frameFlow("receive");
    constexpr size_t InitialFrameCapacity = 1024 * 1024;
    constexpr size_t MaximumFrameCapacity = 64 * 1024 * 1024;
    std::vector<unsigned char> frame(InitialFrameCapacity);
    VideoPacketLossInterval packetLossInterval;
    quint64 nextPacketLossSample = PlankClock::milliseconds();

    const auto sampleTransportTelemetry = [this, &packetLossInterval,
                                           &nextPacketLossSample]() {
        const quint64 now = PlankClock::milliseconds();
        if (now < nextPacketLossSample) {
            return;
        }
        nextPacketLossSample = now + 1000;

        PlankTransportNativeStats stats {};
        stats.struct_size = sizeof(stats);
        if (plank_transport_native_endpoint_stats(m_PlankTransportEndpoint, &stats) !=
                PLANK_TRANSPORT_OK) {
            return;
        }

        const std::uint64_t roundedRttMs =
                (stats.quic_rtt_us + 500) / 1000;
        m_CurrentNetworkRttMs.store(
                    static_cast<std::uint32_t>(std::min<std::uint64_t>(
                        roundedRttMs,
                        std::numeric_limits<std::uint32_t>::max())),
                    std::memory_order_relaxed);

        const auto packetLossPercent = packetLossInterval.addCumulative(
                    stats.video_fec_source_symbols,
                    stats.video_fec_source_symbols_missing,
                    stats.video_fec_source_symbols_unrecovered);
        if (packetLossPercent.has_value()) {
            updateVideoFecLoss(*packetLossPercent);
        }
    };

    while (!m_PlankTransportReceiversStopping.load()) {
        sampleTransportTelemetry();
        PlankTransportNativeVideoFrameInfo info {};
        info.struct_size = sizeof(info);
        size_t frameSize = 0;
        const int result = plank_transport_native_video_receive(
                    m_PlankTransportEndpoint, &info, frame.data(), frame.size(),
                    &frameSize, 50);
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            continue;
        }
        if (result == PLANK_TRANSPORT_ERROR_BUFFER_TOO_SMALL &&
                frameSize > frame.size() &&
                frameSize <= MaximumFrameCapacity) {
            frame.resize(frameSize);
            continue;
        }
        if (result != PLANK_TRANSPORT_OK) {
            if (!m_PlankTransportReceiversStopping.load()) {
                qWarning() << "Native KyProto video receive failed:" << result;
            }
            return;
        }
        if (frameSize > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                info.frame_number > std::numeric_limits<uint32_t>::max()) {
            qWarning() << "Native KyProto video frame metadata is out of range";
            LiRequestIdrFrame();
            continue;
        }

        m_LastPlankVideoReceived.store(PlankClock::milliseconds());
        frameFlow.record(ClientFrameFlowTrace::Receive,
                         (info.pts / 90000) * 1000000 + (info.pts % 90000) * 1000000 / 90000,
                         info.frame_number,
                         (info.flags & PLANK_TRANSPORT_NATIVE_VIDEO_FLAG_KEY) != 0,
                         frameSize);
        const uint32_t flags =
                (info.flags & PLANK_TRANSPORT_NATIVE_VIDEO_FLAG_KEY) != 0 ?
                    PLANK_VIDEO_FRAME_FLAG_KEY : 0;
        const int submitResult = LiSubmitPlankVideoFrame(
                    frame.data(), static_cast<int>(frameSize),
                    static_cast<uint32_t>(info.frame_number), flags,
                    info.pts, info.host_processing_latency);
        if (submitResult < 0) {
            qWarning() << "Native KyProto video frame submission failed:"
                       << submitResult << "frame" << info.frame_number;
            LiRequestIdrFrame();
        }
    }
}

void Session::plankTransportAudioReceiveLoop()
{
    constexpr size_t MaximumAudioPacketSize = 64 * 1024;
    std::vector<unsigned char> packet(MaximumAudioPacketSize);

    while (!m_PlankTransportReceiversStopping.load()) {
        PlankTransportNativeAudioPacketInfo info {};
        info.struct_size = sizeof(info);
        size_t packetSize = 0;
        const int result = plank_transport_native_audio_receive(
                    m_PlankTransportEndpoint, &info, packet.data(), packet.size(),
                    &packetSize, 50);
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            continue;
        }
        if (result != PLANK_TRANSPORT_OK ||
                packetSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
            if (!m_PlankTransportReceiversStopping.load()) {
                qWarning() << "Native KyProto audio receive failed:" << result;
            }
            return;
        }

        const int submitResult = LiSubmitPlankAudioPacket(
                    packetSize == 0 ? nullptr : packet.data(),
                    static_cast<int>(packetSize), info.frame_samples,
                    info.missing_samples, info.pts);
        if (submitResult < 0) {
            qWarning() << "Native KyProto audio packet submission failed:"
                       << submitResult;
        }
    }
}

void Session::plankTransportDataReceiveLoop()
{
    std::vector<unsigned char> packet(PLANK_TRANSPORT_EVENT_MAX_PACKET_SIZE);
    while (!m_PlankTransportReceiversStopping.load()) {
        size_t packetSize = 0;
        const int result = plank_transport_native_data_receive(
                    m_PlankTransportEndpoint, packet.data(), packet.size(),
                    &packetSize, 50);
        if (result == PLANK_TRANSPORT_TIMEOUT) {
            continue;
        }
        if (result != PLANK_TRANSPORT_OK) {
            if (!m_PlankTransportReceiversStopping.load()) {
                qWarning() << "Native KyProto control receive failed:" << result;
                // This receiver owns termination for the shared QUIC connection.
                // Drain queued control records first (notably session takeover),
                // then use the existing duplicate-safe reconnect callback. A
                // closed receive path must not wait for a mouse/key send failure.
                clConnectionTerminated(result);
            }
            return;
        }

        if (packetSize < sizeof(uint32_t)) {
            qWarning() << "Rejected undersized native KyProto data record";
            LiNotifyPlankHostTermination(-1);
            return;
        }
        const uint32_t magic = plank_transport_event_read_u32(packet.data());
        if (magic == PLANK_TRANSPORT_CONTROL_MAGIC) {
            PlankTransportControlPacket control {};
            if (plank_transport_control_decode(
                        packet.data(), packetSize, &control) != 0) {
                qWarning() << "Rejected malformed native KyProto control packet";
                LiNotifyPlankHostTermination(-1);
                return;
            }
            switch (control.type) {
            case PLANK_TRANSPORT_CONTROL_CAMERA_APPLIED:
            case PLANK_TRANSPORT_CONTROL_CAMERA_KEYFRAME: {
                std::uint64_t generation = 0; std::uint32_t value = 0;
                if (!m_CameraNegotiated || plank_camera_control_decode(&control, &generation, &value)) {
                    LiNotifyPlankHostTermination(-1); return;
                }
                std::lock_guard<std::mutex> guard(m_CameraMutex);
                if (m_Camera) {
                    if (control.type == PLANK_TRANSPORT_CONTROL_CAMERA_APPLIED) m_Camera->acknowledge(generation, value);
                    else m_Camera->requestKeyframe(generation);
                }
                break;
            }
            case PLANK_TRANSPORT_CONTROL_MICROPHONE_APPLIED: {
                if (!m_MicrophoneNegotiated || control.payload_size != 12 ||
                        plank_transport_control_read_u32(control.payload + 8) > PLANK_TRANSPORT_MICROPHONE_UNAVAILABLE) {
                    LiNotifyPlankHostTermination(-1); return;
                }
                const uint64_t generation = uint64_t(plank_transport_control_read_u32(control.payload)) << 32 |
                        plank_transport_control_read_u32(control.payload + 4);
                std::lock_guard<std::mutex> guard(m_MicrophoneMutex);
                if (m_Microphone) m_Microphone->acknowledge(generation, plank_transport_control_read_u32(control.payload + 8));
                break;
            }
            case PLANK_TRANSPORT_CONTROL_HOST_DESKTOP_HANDOFF:
                if (control.payload_size != 0 ||
                        !(m_Computer->plankFeatureFlags & NvOutputTopology::DesktopHandoffNoticeFeature)) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                // Advisory only. Do not reconnect until the transport actually
                // closes; a stale notice must not relabel an unrelated outage.
                m_DesktopHandoffNoticeDeadline.store(PlankClock::milliseconds() + 5000);
                qInfo() << "Host announced GDM-to-desktop handoff";
                break;
            case PLANK_TRANSPORT_CONTROL_VIDEO_BITRATE_APPLIED:
                if (control.payload_size != 3 * sizeof(uint32_t)) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                LiNotifyPlankVideoBitrateApplied(
                            plank_transport_control_read_u32(control.payload),
                            plank_transport_control_read_u32(control.payload + 4),
                            plank_transport_control_read_u32(control.payload + 8));
                break;
            case PLANK_TRANSPORT_CONTROL_HOST_TERMINATE:
                if (control.payload_size != sizeof(uint32_t)) {
                    LiNotifyPlankHostTermination(-1);
                    return;
                }
                LiNotifyPlankHostTermination(
                            plank_transport_control_read_u32(control.payload));
                return;
            default:
                qWarning() << "Rejected unexpected native KyProto control type"
                           << control.type;
                LiNotifyPlankHostTermination(-1);
                return;
            }
            continue;
        }

        PlankTransportEventPacket event {};
        if (magic != PLANK_TRANSPORT_EVENT_MAGIC ||
                plank_transport_event_decode(
                    packet.data(), packetSize, &event) != 0) {
            qWarning() << "Rejected malformed native KyProto event record";
            LiNotifyPlankHostTermination(-1);
            return;
        }
        switch (event.type) {
        case PLANK_TRANSPORT_EVENT_HDR_MODE: {
            if (event.payload_size != PLANK_TRANSPORT_EVENT_HDR_MODE_SIZE ||
                    event.payload[1] != 0) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            SS_HDR_METADATA metadata {};
            size_t offset = 2;
            for (int index = 0; index < 3; ++index) {
                metadata.displayPrimaries[index].x =
                        plank_transport_event_read_u16(event.payload + offset);
                metadata.displayPrimaries[index].y =
                        plank_transport_event_read_u16(event.payload + offset + 2);
                offset += 4;
            }
            metadata.whitePoint.x = plank_transport_event_read_u16(event.payload + offset);
            metadata.whitePoint.y = plank_transport_event_read_u16(event.payload + offset + 2);
            offset += 4;
            metadata.maxDisplayLuminance = plank_transport_event_read_u16(event.payload + offset);
            metadata.minDisplayLuminance = plank_transport_event_read_u16(event.payload + offset + 2);
            metadata.maxContentLightLevel = plank_transport_event_read_u16(event.payload + offset + 4);
            metadata.maxFrameAverageLightLevel = plank_transport_event_read_u16(event.payload + offset + 6);
            metadata.maxFullFrameLuminance = plank_transport_event_read_u16(event.payload + offset + 8);
            LiNotifyPlankHdrMode(event.payload[0] != 0, &metadata);
            break;
        }
        case PLANK_TRANSPORT_EVENT_RAW_HID_WACOM:
            if (event.payload_size < sizeof(PLANK_RAW_HID_WIRE_HEADER) ||
                    event.payload_size > sizeof(PLANK_RAW_HID_WIRE_HEADER) +
                        PLANK_RAW_HID_MAX_PAYLOAD_SIZE) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            LiNotifyPlankRawHidControl(
                        event.payload, event.payload_size);
            break;
        case PLANK_TRANSPORT_EVENT_CURSOR_SHAPE:
            if (event.payload_size < sizeof(PLANK_CURSOR_WIRE_HEADER) ||
                    event.payload_size > sizeof(PLANK_CURSOR_WIRE_HEADER) +
                        PLANK_CURSOR_MAX_CHUNK_SIZE) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            LiNotifyPlankCursorChunk(
                        event.payload, event.payload_size);
            break;
        case PLANK_TRANSPORT_EVENT_CURSOR_POSITION:
            if (event.payload_size != sizeof(PLANK_CURSOR_POSITION_WIRE_MESSAGE)) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
            LiNotifyPlankCursorPosition(
                        event.payload, event.payload_size);
            break;
        case PLANK_TRANSPORT_EVENT_CLIPBOARD_OFFER:
            if (event.payload_size < sizeof(PLANK_CLIPBOARD_WIRE_HEADER) ||
                    event.payload_size > sizeof(PLANK_CLIPBOARD_WIRE_HEADER) +
                        PLANK_CLIPBOARD_MAX_EVENT_CHUNK_SIZE) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
#ifdef Q_OS_MACOS
            if (m_ClipboardSync && clipboardSyncEnabled() &&
                    !m_ClipboardSync->handleHostOffer(event.payload, event.payload_size)) {
                LiNotifyPlankHostTermination(-1);
                return;
            }
#endif
            break;
        default:
            qWarning() << "Rejected unexpected native KyProto event type"
                       << event.type;
            LiNotifyPlankHostTermination(-1);
            return;
        }
    }
}

int Session::plankTransportNativeControlSender(void* context, uint32_t type,
                                          uint32_t value1, uint32_t value2)
{
    if (context == nullptr) {
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    uint16_t wireType = 0;
    uint32_t values[2] {value1, value2};
    size_t valueCount = 0;
    switch (type) {
    case LI_SC_NATIVE_CONTROL_REQUEST_IDR:
        wireType = PLANK_TRANSPORT_CONTROL_REQUEST_IDR;
        break;
    case LI_SC_NATIVE_CONTROL_INVALIDATE_REFERENCE_FRAMES:
        wireType = PLANK_TRANSPORT_CONTROL_INVALIDATE_REFERENCE_FRAMES;
        valueCount = 2;
        break;
    case LI_SC_NATIVE_CONTROL_SET_VIDEO_BITRATE:
        wireType = PLANK_TRANSPORT_CONTROL_SET_VIDEO_BITRATE;
        valueCount = 1;
        break;
    default:
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }

    unsigned char packet[PLANK_TRANSPORT_CONTROL_MAX_PACKET_SIZE];
    size_t packetSize = 0;
    if (plank_transport_control_encode(
                wireType, values, valueCount, packet, sizeof(packet),
                &packetSize) != 0) {
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    return plank_transport_native_data_send(
                static_cast<PlankTransportNativeEndpoint*>(context), packet,
                packetSize);
}

int Session::plankTransportNativeInputSender(void* context, uint8_t type,
                                        const unsigned char* payload,
                                        size_t payloadLength)
{
    if (context == nullptr || payload == nullptr || payloadLength == 0) {
        return PLANK_TRANSPORT_ERROR_INVALID_ARGUMENT;
    }
    return plank_transport_native_input_send(
                static_cast<PlankTransportNativeEndpoint*>(context), type,
                payload, payloadLength);
}
#endif

#ifdef Q_OS_MACOS
bool Session::clipboardSyncEnabled() const
{
    return m_Computer != nullptr &&
            (!(m_Computer->plankFeatureFlags & NvOutputTopology::FixedCaptureFeature) || m_MacClipboardNegotiated) &&
            (m_Computer->plankFeatureFlags & NvOutputTopology::ClipboardSyncFeature) != 0;
}

void Session::createClipboardSync()
{
#ifdef PLANK_TRANSPORT
    if (!m_ClipboardSync) {
        m_ClipboardSync = std::make_unique<MacClipboardSync>(
                    [this](const std::uint8_t* payload, std::size_t size) {
                        return plankTransportNativeInputSender(
                                   m_PlankTransportEndpoint,
                                   PLANK_TRANSPORT_INPUT_CLIPBOARD_OFFER,
                                   payload,
                                   size) == PLANK_TRANSPORT_OK;
                    },
                    [this] {
                        return m_QtClipboardFocused.load();
                    },
                    [this] { return clipboardSyncEnabled(); },
                    [] {
                        // Qt's session timer drains the bounded pending value.
                        return true;
                    });
    }
#endif
}

void Session::startClipboardSync()
{
#ifdef PLANK_TRANSPORT
    if (!clipboardSyncEnabled() || m_PlankTransportEndpoint == nullptr) {
        return;
    }
    createClipboardSync();
    m_ClipboardSync->start();
    qInfo() << "Started PLANK clipboard sync";
#endif
}

void Session::stopClipboardSync()
{
    if (m_ClipboardSync) {
        m_ClipboardSync->stop();
    }
}

#endif

void Session::clearPlankReconnectCredentials()
{
    m_CanReconnect.store(false);
    m_PlankPassword.fill(QChar('\0'));
    m_PlankPassword.clear();
    m_PlankUsername.clear();
}

bool Session::initialize(IFFmpegRendererFactory* presenter)
{
    Q_ASSERT(presenter);
    m_RendererFactory = presenter;

    if (!StreamingPreferences::isPlankProfileValidForCaptureSource(
                m_PlankVideoProfile,
                m_PlankCaptureSource)) {
        const QString error = tr("The selected capture source and encoding profile are not compatible.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_NVFBC_8BIT &&
            m_PlankVideoProfile ==
                StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444 &&
            (m_Computer->plankFeatureFlags &
             NvOutputTopology::NvfbcHevc10NvencFeature) == 0) {
        const QString error = tr("The host does not support NvFBC with the HEVC 10-bit NVENC profile.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }

    LiInitializeStreamConfiguration(&m_StreamConfig);
    if (!configurePlankLaunchGeometry()) {
        return false;
    }

    qInfo() << "PLANK host version:"
            << m_Computer->plankHostVersion;

    LiInitializeVideoCallbacks(&m_VideoCallbacks);
    m_VideoCallbacks.setup = drSetup;

    m_StreamConfig.fps = m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT ?
                60 : m_Preferences->fps;
    m_StreamConfig.bitrate = m_PlankBitrateKbps;

    qInfo("Video bitrate: %d kbps",
                m_StreamConfig.bitrate);

    switch (m_Preferences->audioConfig)
    {
    case StreamingPreferences::AC_STEREO:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
        break;
    case StreamingPreferences::AC_51_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
        break;
    case StreamingPreferences::AC_71_SURROUND:
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
        break;
    }
    if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
        // The authenticated Mac contract is system audio, stereo 48 kHz.
        m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    }

    LiInitializeAudioCallbacks(&m_AudioCallbacks);
    m_AudioCallbacks.init = arInit;
    m_AudioCallbacks.cleanup = arCleanup;
    m_AudioCallbacks.decodeAndPlaySample = arDecodeAndPlaySample;
    m_AudioCallbacks.capabilities = getAudioRendererCapabilities(m_StreamConfig.audioConfiguration);

    qInfo("Audio channel count: %d",
                CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));
    qInfo("Audio channel mask: %X",
                CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration));

    // PLANK advertises exactly the selected profile. Do not silently
    // substitute another bit depth or chroma format when probing fails.
    int selectedVideoFormat = VIDEO_FORMAT_H264_HIGH10_444;
    switch (m_PlankVideoProfile) {
    case StreamingPreferences::PLANK_PROFILE_H264_8BIT_422:
        selectedVideoFormat = VIDEO_FORMAT_H264_HIGH8_422;
        break;
    case StreamingPreferences::PLANK_PROFILE_H264_8BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H264_HIGH8_444;
        break;
    case StreamingPreferences::PLANK_PROFILE_H264_10BIT_422:
        selectedVideoFormat = VIDEO_FORMAT_H264_HIGH10_422;
        break;
    case StreamingPreferences::PLANK_PROFILE_H264_10BIT_444:
        break;
    case StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H264_HIGH8_444;
        break;
    case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H265_REXT8_444;
        break;
    case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H265_REXT10_444;
        break;
    case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420:
        selectedVideoFormat = VIDEO_FORMAT_H265_MAIN10;
        break;
    case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444:
        selectedVideoFormat = VIDEO_FORMAT_H265_REXT10_444;
        break;
    default:
        emit displayLaunchError(tr("The bookmark contains an invalid encoding profile."));
        return false;
    }
    if (!(selectedVideoFormat & VIDEO_FORMAT_MASK_YUV444) ||
            isIdentityGbrEnabledForFormat(selectedVideoFormat) ||
            StreamingPreferences::isPlankAppleProfile(m_PlankVideoProfile)) {
        m_SupportedVideoFormats.append(selectedVideoFormat);
    }

    Q_ASSERT(m_SupportedVideoFormats.size() == 1);

    // Check for validation errors/warnings and emit
    // signals for them, if appropriate
    bool ret = validateLaunch();

    if (ret) {
        // Video format is now locked in
        m_StreamConfig.supportedVideoFormats = m_SupportedVideoFormats.front();

        // Populate decoder-dependent properties.
        // Must be done after validateLaunch() since m_StreamConfig is finalized.
        ret = populateDecoderProperties();
    }

    if (!ret) {
        return false;
    }

    return true;
}

void Session::emitLaunchWarning(QString text)
{
    // Emit the warning to the UI
    emit displayLaunchWarning(text);

    // Wait a little bit so the user can actually read what we just said.
    // This wait is a little longer than the actual toast timeout (3 seconds)
    // to allow it to transition off the screen before continuing.
    const quint64 start = PlankClock::milliseconds();
    while (!m_ConnectionStartCancelled.load() && PlankClock::milliseconds() < start + 3500) {
        QThread::msleep(5);

    }
}

bool Session::validateLaunch()
{
    m_SupportedVideoFormats.removeByMask(
                ~m_SupportedVideoFormats.maskByServerCodecModes(m_Computer->serverCodecModeSupport));
    if (m_SupportedVideoFormats.isEmpty()) {
        emit displayLaunchError(tr("The selected PLANK encoding profile is not supported by both this host and client."));
        return false;
    }

    // Internal exact-profile decoder selection may legitimately choose
    // software when no hardware path reproduces the selected profile. That is
    // an expected capability result, so do not interrupt each connection with
    // a warning.
    while (!m_SupportedVideoFormats.isEmpty()) {
        const auto availability = getDecoderAvailability(
                    m_SupportedVideoFormats.front(),
                    m_StreamConfig.width,
                    m_StreamConfig.height,
                    m_StreamConfig.fps,
                    isIdentityGbrEnabledForFormat(m_SupportedVideoFormats.front()));
        if (availability == DecoderAvailability::None) {
            m_SupportedVideoFormats.removeFirst();
        }
        else {
            break;
        }
    }
    if (m_SupportedVideoFormats.isEmpty()) {
        emit displayLaunchError(tr("This client cannot decode the selected PLANK encoding profile."));
        return false;
    }

    // Test if audio works at the specified audio configuration
    bool audioTestPassed = testAudio(m_StreamConfig.audioConfiguration);

    // Gracefully degrade to stereo if surround sound doesn't work
    if (!audioTestPassed && CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(m_StreamConfig.audioConfiguration) > 2) {
        audioTestPassed = testAudio(AUDIO_CONFIGURATION_STEREO);
        if (audioTestPassed) {
            m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
            emitLaunchWarning(tr("Your selected surround sound setting is not supported by the current audio device."));
        }
    }

    // If nothing worked, warn the user that audio will not work
    if (!audioTestPassed) {
        emitLaunchWarning(tr("Failed to open audio device. Audio will be unavailable during this session."));
    }

    return true;
}

bool Session::configurePlankHostLayout()
{
    QString layoutPolicy;
    QString scalingMode;
    QString virtualMode1;
    QString virtualMode2;
    QSize authenticatedDesktopSize;
    QSizeF authenticatedLogicalSize;
    bool hostRejectsRequestedLayout = false;
    bool virtualPrimary = false;
    {
        QReadLocker lock(&m_Computer->lock);
        layoutPolicy = m_Computer->plankHostLayout;
        scalingMode = m_Computer->plankScalingMode;
        virtualMode1 = m_Computer->plankVirtualMode1;
        virtualMode2 = m_Computer->plankVirtualMode2;
        authenticatedDesktopSize = QSize(m_Computer->outputTopology.desktopWidth,
                                         m_Computer->outputTopology.desktopHeight);
        authenticatedLogicalSize = m_Computer->outputTopology.captureLogicalBounds.size();
        virtualPrimary = m_Computer->outputTopology.startupLayoutKind == NvOutputTopology::SingleHostLayout &&
            (m_Computer->outputTopology.featureFlags & NvOutputTopology::VirtualPrimaryConnectorFeature);
        const bool hostPolicyKnown = m_Computer->outputTopology.displayPolicyKnown();
        hostRejectsRequestedLayout = hostPolicyKnown &&
                !m_Computer->outputTopology.allowsBookmarkHostLayout(layoutPolicy);
    }

    if (hostRejectsRequestedLayout) {
        const QString error = tr("This workstation does not support the display layout selected by the bookmark.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }

    m_ResolvedHostLayout.clear();
    m_ResolvedVirtualModes.clear();
    m_ResolvedPrimaryOutput = -1;
    if (scalingMode != NvOutputTopology::NativeScalingMode &&
            scalingMode != NvOutputTopology::ScaledSpanMode) {
        const QString error = tr("The bookmark contains an unsupported client scaling mode.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }
    m_ResolvedScalingMode = scalingMode;
    if (layoutPolicy == NvOutputTopology::MatchClientHostLayout) {
        QVector<NvClientDisplay> displays;
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            displays.append({display.macMatchedBounds.isValid() ? display.macMatchedBounds : QRect(display.logicalBounds.x(),
                                   display.logicalBounds.y(),
                                   display.logicalBounds.width(),
                                   display.logicalBounds.height()),
                             display.nativeSize, display.macBackingSize, display.primary});
        }

        QString error;
        if (m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT) {
            int scale = 1;
            const QString mode = NvOutputTopology::resolveMacClientDisplayMode(displays, &error, &scale);
            if (mode.isEmpty() || NvOutputTopology::macDisplayModeSize(mode) !=
                    authenticatedDesktopSize || authenticatedLogicalSize !=
                    QSizeF(authenticatedDesktopSize.width() / scale, authenticatedDesktopSize.height() / scale)) {
                emit displayLaunchError(mode.isEmpty() ? error : tr("Client displays changed during connection. Please reconnect to match the current display resolution."));
                return false;
            }
            m_ResolvedHostLayout = QStringLiteral("fixed");
        }
        else if (!NvOutputTopology::resolveClientDisplayLayout(
                    displays, m_ResolvedHostLayout, m_ResolvedVirtualModes, &error,
                    virtualPrimary ? &m_ResolvedPrimaryOutput : nullptr)) {
            qCritical("%s", qPrintable(error));
            emit displayLaunchError(error);
            return false;
        }
    }
    else if (layoutPolicy == QStringLiteral("fixed") &&
             m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT &&
             m_Computer->outputTopology.featureFlags == NvOutputTopology::FixedCaptureFlags) {
        m_ResolvedHostLayout = layoutPolicy;
    }
    else if (layoutPolicy == NvOutputTopology::PhysicalHostLayout) {
        m_ResolvedHostLayout = NvOutputTopology::PhysicalHostLayout;
    }
    else if (layoutPolicy == NvOutputTopology::SingleHostLayout ||
             layoutPolicy == NvOutputTopology::DualHorizontalHostLayout) {
        if (!NvOutputTopology::qualifiedVirtualModes().contains(virtualMode1) ||
                (layoutPolicy == NvOutputTopology::DualHorizontalHostLayout &&
                 !NvOutputTopology::qualifiedVirtualModes().contains(virtualMode2))) {
            const QString error = tr("The bookmark contains an unsupported virtual monitor resolution.");
            qCritical("%s", qPrintable(error));
            emit displayLaunchError(error);
            return false;
        }
        m_ResolvedHostLayout = layoutPolicy;
        m_ResolvedVirtualModes.append(virtualMode1);
        if (layoutPolicy == NvOutputTopology::DualHorizontalHostLayout) {
            m_ResolvedVirtualModes.append(virtualMode2);
            if (virtualPrimary) {
                QVector<NvClientDisplay> displays;
                for (const auto& display : std::as_const(m_ClientDisplays)) {
                    displays.append({QRect(display.logicalBounds.x(), display.logicalBounds.y(),
                                           display.logicalBounds.width(), display.logicalBounds.height()),
                                     display.nativeSize, display.macBackingSize, display.primary});
                }
                // A manual bookmark remains usable with a different number
                // or arrangement of local monitors. Omit an ambiguous hint;
                // never send an index into unrelated local displays.
                m_ResolvedPrimaryOutput = NvOutputTopology::clientPrimaryIndex(
                    displays, m_ResolvedVirtualModes.size());
            }
        }
    }
    else {
        const QString error = tr("The bookmark contains an unsupported host display layout.");
        qCritical("%s", qPrintable(error));
        emit displayLaunchError(error);
        return false;
    }

    qInfo("PLANK host layout: policy=%s resolved=%s modes=%s scaling=%s",
                qPrintable(layoutPolicy), qPrintable(m_ResolvedHostLayout),
                qPrintable(m_ResolvedVirtualModes.join(',')),
                qPrintable(m_ResolvedScalingMode));
    return true;
}

QSize Session::configurePlankDisplayMode()
{
    QSize detectedResolution;

    if (m_UseMultiDisplayPresentation) {
        int width = 0;
        int height = 0;
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            width += display.nativeSize.width();
            height = qMax(height, display.nativeSize.height());
        }
        detectedResolution = QSize(width, height);
    }
    else {
        for (const auto& display : std::as_const(m_ClientDisplays)) {
            if (display.displayId == m_TargetDisplayId) {
                detectedResolution = display.nativeSize;
                break;
            }
        }
    }

    QSize nativeCanvasResolution;
    {
        QReadLocker lock(&m_Computer->lock);
        if (m_ResolvedHostLayout == NvOutputTopology::PhysicalHostLayout ||
                m_ResolvedHostLayout == QStringLiteral("fixed")) {
            nativeCanvasResolution = QSize(m_Computer->outputTopology.desktopWidth,
                                           m_Computer->outputTopology.desktopHeight);
        }
    }
    if (m_ResolvedHostLayout != NvOutputTopology::PhysicalHostLayout &&
            m_ResolvedHostLayout != QStringLiteral("fixed")) {
        nativeCanvasResolution = NvOutputTopology::virtualCanvasSize(
                    m_ResolvedHostLayout, m_ResolvedVirtualModes);
    }

    QSize selectedResolution;
    if (m_ResolvedScalingMode == NvOutputTopology::NativeScalingMode ||
            m_ResolvedHostLayout == QStringLiteral("fixed")) {
        if (!nativeCanvasResolution.isValid()) {
            const QString error = tr("Native scaling requires a valid host desktop pixel size.");
            qCritical("%s", qPrintable(error));
            emit displayLaunchError(error);
            return QSize();
        }
        selectedResolution = nativeCanvasResolution;
    }
    else {
        selectedResolution = PlankDisplayMode::resolve(
            detectedResolution, nativeCanvasResolution);
    }
    qInfo("PLANK client physical resolution: detected=%dx%d host-native=%dx%d selected=%dx%d scaling=%s resolution-policy=%s",
                detectedResolution.width(), detectedResolution.height(),
                nativeCanvasResolution.width(), nativeCanvasResolution.height(),
                selectedResolution.width(), selectedResolution.height(),
                qPrintable(m_ResolvedScalingMode),
                m_ResolvedScalingMode == NvOutputTopology::NativeScalingMode ?
                    "host-native" : "client-native");
    return selectedResolution;
}

bool Session::configurePlankLaunchGeometry()
{
    if (m_Computer->plankAuthentication &&
            !configurePlankHostLayout()) {
        return false;
    }

    {
        QReadLocker lock(&m_Computer->lock);
        const int hostOutputs = m_Computer->outputTopology.outputCountForLayout(m_ResolvedHostLayout);
        m_UseMultiDisplayPresentation = m_MultiDisplayPresentationAvailable;
#ifdef Q_OS_DARWIN
        // The Mac single-output policy is qualified independently of Wayland.
        m_UseMultiDisplayPresentation = m_UseMultiDisplayPresentation && hostOutputs > 1;
#endif
        qInfo("PLANK presentation selection: host-outputs=%d client-multi=%d selected-outputs=%d",
                    hostOutputs, m_MultiDisplayPresentationAvailable,
                    m_UseMultiDisplayPresentation ? 2 : 1);
    }

    const QSize resolution = configurePlankDisplayMode();
    if (!resolution.isValid()) {
        return false;
    }

    const QSize previousResolution(m_StreamConfig.width,
                                   m_StreamConfig.height);
    m_StreamConfig.width = resolution.width();
    m_StreamConfig.height = resolution.height();

    if (previousResolution.isValid() && previousResolution != resolution) {
        qInfo() << "PLANK launch geometry changed from"
                << previousResolution << "to" << resolution;
    }
    return true;
}

class AsyncConnectionStartThread : public QThread
{
public:
    AsyncConnectionStartThread(Session* session) :
        QThread(nullptr),
        m_Session(session)
    {
        setObjectName("Async Conn Start");
    }

    void run() override
    {
        m_Session->m_AsyncConnectionSuccess = m_Session->startConnectionAsync();
    }

    Session* m_Session;
};

// Called in a non-main thread
bool Session::startConnectionAsync(bool reconnecting,
                                   bool takeOverActiveSession)
{
    // Wait 1.5 seconds before connecting to let the user
    // have time to read any messages present on the segue
    if (!reconnecting && !takeOverActiveSession) {
        for (int elapsed = 0; elapsed < 1500; elapsed += 50) {
            if (m_ConnectionStartCancelled.load()) return false;
            QThread::msleep(50);
        }
    }

    // PLANK never terminates a host application remotely. Only resume
    // the already-running Desktop application or launch it from an idle host.
    Q_ASSERT(m_Computer->currentGameId == 0 ||
             m_Computer->currentGameId == m_App.id);

    quint16 plankTransportPort = 0;
    QString plankTransportCertificateSha256;
    QString plankTransportToken;
    QString acceptedCaptureSource;
    QString acceptedEncoderBackend;
    QString acceptedEncodingMode;
    const bool macCapture = m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT;
    MacPreviewLaunch::Reply macLaunch;
    quint32 routeInterfaceMtu = 0;
    bool routeIsIpv6 = false;
    const NvComputer::ReachabilityType routeReachability =
            m_Computer->getActiveAddressReachability(&routeInterfaceMtu, &routeIsIpv6);
    const int configuredMtu = m_Preferences->quicUdpPayloadMtu;
    const quint16 quicUdpPayloadMtu = PlankNetwork::quicUdpPayloadMtuForRoute(
                configuredMtu, routeReachability == NvComputer::RI_ZEROTIER,
                routeInterfaceMtu, routeIsIpv6);
    if (quicUdpPayloadMtu == 0) {
        const quint32 overhead = routeIsIpv6 ? PlankNetwork::InnerIpv6UdpOverhead :
                                              PlankNetwork::InnerIpv4UdpOverhead;
        const quint32 minimumMtu = PlankNetwork::MinimumQuicUdpPayloadMtu +
                overhead + PlankNetwork::AutomaticPathSafetyMargin;
        if (routeInterfaceMtu && routeInterfaceMtu < minimumMtu) {
            emit displayLaunchError(tr("The network interface MTU is %1 bytes. PLANK requires at least %2 bytes for %3, including protocol headers and its safety margin.")
                                    .arg(routeInterfaceMtu).arg(minimumMtu)
                                    .arg(routeIsIpv6 ? QStringLiteral("IPv6") : QStringLiteral("IPv4")));
        }
        else {
            emit displayLaunchError(tr("The configured QUIC UDP payload (%1 bytes) is invalid or exceeds the network interface's safe payload limit. Choose Automatic or a smaller value of at least 1200 bytes.")
                                    .arg(configuredMtu));
        }
        qWarning() << "Rejected QUIC UDP payload: configured=" << configuredMtu
                   << "interface MTU=" << routeInterfaceMtu
                   << (routeIsIpv6 ? "IPv6" : "IPv4");
        return false;
    }
    qInfo() << (configuredMtu ? "Manual fixed QUIC UDP payload ceiling:" :
                              "Automatically resolved fixed QUIC UDP payload ceiling:")
            << quicUdpPayloadMtu << "bytes from interface MTU" << routeInterfaceMtu
            << (routeIsIpv6 ? "(IPv6)" : "(IPv4)")
            << "ZeroTier=" << (routeReachability == NvComputer::RI_ZEROTIER);

    try {
        std::unique_ptr<NvHTTP> http = std::make_unique<NvHTTP>(m_Computer);
        if (reconnecting) http->setRequestGate([this](bool auth) { return waitForPlankReconnectRequest(auth); });
        const QString captureSource =
                m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_SCREENCAPTUREKIT ?
                    QStringLiteral("screencapturekit") :
                m_PlankCaptureSource == StreamingPreferences::PLANK_CAPTURE_X11_NATIVE10 ?
                    QStringLiteral("x11-native10") : QStringLiteral("nvfbc");
        const QString encoderBackend =
                StreamingPreferences::isPlankAppleProfile(m_PlankVideoProfile) ?
                    QStringLiteral("videotoolbox") :
                StreamingPreferences::isPlankNvencProfile(
                    m_PlankVideoProfile) ?
                    QStringLiteral("nvenc-direct") : QStringLiteral("software-cuda");
        QString encodingMode;
        switch (m_PlankVideoProfile) {
        case StreamingPreferences::PLANK_PROFILE_H264_8BIT_422:
            encodingMode = QStringLiteral("h264-8-422-software");
            break;
        case StreamingPreferences::PLANK_PROFILE_H264_8BIT_444:
            encodingMode = QStringLiteral("h264-8-444-software");
            break;
        case StreamingPreferences::PLANK_PROFILE_H264_10BIT_422:
            encodingMode = QStringLiteral("h264-10-422-software");
            break;
        case StreamingPreferences::PLANK_PROFILE_NVENC_H264_8BIT_444:
            encodingMode = QStringLiteral("h264-8-444-nvenc");
            break;
        case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_8BIT_444:
            encodingMode = QStringLiteral("hevc-8-444-nvenc");
            break;
        case StreamingPreferences::PLANK_PROFILE_NVENC_HEVC_10BIT_444:
            encodingMode = QStringLiteral("hevc-10-444-nvenc");
            break;
        case StreamingPreferences::PLANK_PROFILE_H264_10BIT_444:
            encodingMode = QStringLiteral("h264-10-444-software");
            break;
        case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_420:
            encodingMode = QStringLiteral("hevc-10-420-videotoolbox");
            break;
        case StreamingPreferences::PLANK_PROFILE_APPLE_HEVC_10BIT_444:
            encodingMode = QStringLiteral("hevc-10-444-videotoolbox");
            break;
        default:
            emit displayLaunchError(tr("The bookmark contains an invalid encoding profile."));
            return false;
        }
        const auto startApp = [&]() {
            if (macCapture) {
                QString pin;
                const NvOutputTopology topology = http->getOutputTopology(&pin);
                if (topology.toJson() != m_Computer->outputTopology.toJson() ||
                        topology.desktopWidth != m_StreamConfig.width ||
                        topology.desktopHeight != m_StreamConfig.height) {
                    throw GfeHttpResponseException(409, "The Mac display changed. Reconnect to refresh its capture geometry.");
                }
                // Launch is one-shot, including an ambiguous network timeout.
                // Readiness failures above keep the setup context instead.
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->sessionToken.fill(QChar('\0'));
                    m_Computer->sessionToken.clear();
                    m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
                }
                macLaunch = http->startMacPreview(topology, pin, m_StreamConfig.bitrate, quicUdpPayloadMtu);
                m_MacClipboardNegotiated = macLaunch.clipboard;
                m_MicrophoneNegotiated = macLaunch.microphone;
                m_MicrophoneSchema = macLaunch.microphoneSchema;
                m_CameraNegotiated = macLaunch.camera;
                plankTransportPort = http->controlPort();
                plankTransportCertificateSha256 = pin;
                plankTransportToken = QString::fromLatin1(macLaunch.transportToken);
                macLaunch.transportToken.fill('\0');
                macLaunch.transportToken.clear();
                acceptedCaptureSource = captureSource;
                acceptedEncoderBackend = encoderBackend;
                acceptedEncodingMode = encodingMode;
                return;
            }
            http->startApp(m_Computer->currentGameId != 0 ? "resume" : "launch",
                          m_App.id, &m_StreamConfig,
                          m_Preferences->playAudioOnHost,
                          0,
                          false,
                          NvOutputTopology::ScaledSpanMode,
                          m_Computer->outputTopology.generation,
                          m_Computer->plankTopologyVersion,
                          m_Computer->plankFeatureFlags &
                              NvOutputTopology::SupportedFeatureFlags,
                          takeOverActiveSession,
                          m_ResolvedHostLayout,
                          m_ResolvedVirtualModes.value(0),
                          m_ResolvedVirtualModes.value(1),
                          captureSource,
                          encoderBackend,
                          encodingMode,
                          quicUdpPayloadMtu,
                          plankTransportPort,
                          plankTransportCertificateSha256,
                          plankTransportToken,
                          acceptedCaptureSource,
                          acceptedEncoderBackend,
                          acceptedEncodingMode,
                          m_ResolvedPrimaryOutput);
        };
        try {
            startApp();
        } catch (const GfeHttpResponseException& e) {
            const QString statusMessage = QString::fromUtf8(e.getStatusMessage());
            const bool displayTransitionStarted =
                    m_Computer->plankAuthentication &&
                    ((e.getStatusCode() == 425 &&
                      statusMessage ==
                          QStringLiteral("PLANK host display transition started")) ||
                     (takeOverActiveSession &&
                      e.getStatusCode() == 503 &&
                      statusMessage ==
                          QStringLiteral("Host display layout transition is currently unavailable")));
            const bool activeSessionConflict =
                    m_Computer->plankAuthentication &&
                    e.getStatusCode() == 409 &&
                    QString::fromUtf8(e.getStatusMessage()) ==
                        QStringLiteral("PLANK workstation session is active");
            if (displayTransitionStarted) {
                constexpr int RetryIntervalMs = 500;
                constexpr int MaximumWaitMs = 45000;
                constexpr int CancellationPollMs = 50;
                bool started = false;

                if (m_PlankUsername.isEmpty() ||
                        m_PlankPassword.isEmpty()) {
                    throw;
                }
                m_WaitingForSessionCleanup.store(true);
                emit sessionCleanupWaitChanged(
                            true,
                            tr("Applying workstation display layout..."));
                qInfo() << "PLANK host display transition started; waiting up to"
                        << MaximumWaitMs << "ms";
                bool authenticationRefreshRequired = false;

                for (int elapsedMs = 0;
                     elapsedMs < MaximumWaitMs && !started;
                     elapsedMs += RetryIntervalMs) {
                    for (int delayMs = 0;
                         delayMs < RetryIntervalMs;
                         delayMs += CancellationPollMs) {
                        if (m_ConnectionStartCancelled.load()) break;
                        QThread::msleep(CancellationPollMs);
                    }
                    if (m_ConnectionStartCancelled.load()) break;

                    if (authenticationRefreshRequired) {
                        try {
                            {
                                QWriteLocker lock(&m_Computer->lock);
                                m_Computer->sessionToken.fill(QChar('\0'));
                                m_Computer->sessionToken.clear();
                                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
                                m_Computer->currentGameId = 0;
                            }
                            http = std::make_unique<NvHTTP>(m_Computer);
                            if (reconnecting) http->setRequestGate([this](bool auth) { return waitForPlankReconnectRequest(auth); });
                            const QString token = http->authenticate(
                                        m_PlankUsername,
                                        m_PlankPassword, nullptr, NvHTTP::AuthenticationIntent::Recovery);
                            {
                                QWriteLocker lock(&m_Computer->lock);
                                m_Computer->sessionToken = token;
                                m_Computer->sessionIdentityKey = http->hostIdentityKey();
                                m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                            }
                            authenticationRefreshRequired = false;
                            qInfo() << "PLANK authenticated to the replacement display worker";
                        } catch (const GfeHttpResponseException& retryError) {
                            if (reconnecting && PlankReconnectPolicy::terminalStatus(retryError.getStatusCode(), true))
                                m_ReconnectCancelled.store(true);
                            throw;
                        } catch (const QtNetworkReplyException& retryError) {
                            if (retryError.getError() == QNetworkReply::SslHandshakeFailedError) throw;
                            qInfo() << "PLANK replacement display worker is not ready for authentication:"
                                    << retryError.toQString();
                            continue;
                        }
                    }

                    try {
                        const NvOutputTopology topology = http->getOutputTopology();
                        {
                            QWriteLocker lock(&m_Computer->lock);
                            m_Computer->outputTopology = topology;
                        }
                        if (m_ComputerManager != nullptr) {
                            m_ComputerManager->clientSideAttributeUpdated(m_Computer);
                        }
                        if (!configurePlankLaunchGeometry()) {
                            m_WaitingForSessionCleanup.store(false);
                            emit sessionCleanupWaitChanged(false, QString());
                            return false;
                        }
                        if (!topology.matchesRequestedHostLayout(
                                    m_ResolvedHostLayout,
                                    m_ResolvedVirtualModes)) {
                            qInfo() << "PLANK display transition is still pending:"
                                    << topology.layoutKind << topology.virtualModes;
                            continue;
                        }
                        startApp();
                        started = true;
                    } catch (const GfeHttpResponseException& retryError) {
                        if (retryError.getStatusCode() == 423) {
                            m_WaitingForSessionCleanup.store(false);
                            emit sessionCleanupWaitChanged(false, QString());
                            throw;
                        }
                        if (retryError.getStatusCode() == 401) {
                            authenticationRefreshRequired = true;
                            qInfo() << "PLANK display worker changed; authentication will be refreshed once";
                            continue;
                        }
                        if (retryError.getStatusCode() != 409 &&
                                retryError.getStatusCode() != 425 &&
                                retryError.getStatusCode() != 503) {
                            m_WaitingForSessionCleanup.store(false);
                            emit sessionCleanupWaitChanged(false, QString());
                            throw;
                        }
                        qInfo() << "PLANK display transition wait attempt failed:"
                                << retryError.toQString();
                    } catch (const QtNetworkReplyException& retryError) {
                        if (retryError.getError() == QNetworkReply::SslHandshakeFailedError) throw;
                        qInfo() << "PLANK display transition worker is not ready:"
                                << retryError.toQString();
                    }
                }

                m_WaitingForSessionCleanup.store(false);
                emit sessionCleanupWaitChanged(false, QString());
                if (!started && m_ConnectionStartCancelled.load()) {
                    qInfo() << "PLANK connection cancelled during display transition";
                    return false;
                }
                if (!started) {
                    if (!reconnecting) {
                        emit displayLaunchError(
                                    tr("The workstation display layout did not become ready within 45 seconds."));
                    }
                    return false;
                }
                qInfo() << "PLANK display transition completed; launch succeeded";
            }
            else if (activeSessionConflict) {
                if (reconnecting) {
                    m_CanReconnect.store(false);
                    m_ReconnectCancelled.store(true);
                    emit displayLaunchError(
                                tr("This PLANK session was transferred to another client."));
                    qInfo() << "PLANK reconnect stopped because another client owns the active session";
                    return false;
                }
                if (takeOverActiveSession ||
                        (m_Computer->plankFeatureFlags &
                         NvOutputTopology::SessionTakeoverFeature) == 0) {
                    emit displayLaunchError(
                                tr("The workstation has an active PLANK session that cannot be transferred."));
                    return false;
                }

                constexpr int DecisionPollMs = 50;
                m_ActiveSessionTakeoverDecision.store(0);
                m_WaitingForActiveSessionTakeoverDecision.store(true);
                emit activeSessionTakeoverRequested(
                            tr("This workstation already has an active PLANK session. Disconnect the existing client and continue?"));
                while (m_ActiveSessionTakeoverDecision.load() == 0 &&
                       !m_ConnectionStartCancelled.load()) {
                    QThread::msleep(DecisionPollMs);
                }
                m_WaitingForActiveSessionTakeoverDecision.store(false);
                if (m_ActiveSessionTakeoverDecision.exchange(0) != 1 ||
                        m_ConnectionStartCancelled.load()) {
                    qInfo() << "PLANK active-session takeover was cancelled";
                    return false;
                }

                m_WaitingForSessionCleanup.store(true);
                emit sessionCleanupWaitChanged(
                            true, tr("Transferring workstation session..."));
                const bool transferred = startConnectionAsync(false, true);
                m_WaitingForSessionCleanup.store(false);
                emit sessionCleanupWaitChanged(false, QString());
                return transferred;
            }
            else if (reconnecting && m_Computer->plankAuthentication &&
                    m_Computer->currentGameId == 0 &&
                    e.getStatusCode() == 400) {
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->currentGameId = m_App.id;
                }
                qInfo() << "PLANK worker already has an active Desktop stream; resuming it";
                startApp();
            }
            else if (reconnecting && m_Computer->plankAuthentication &&
                    m_Computer->currentGameId != 0 &&
                    e.getStatusCode() == 503) {
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->currentGameId = 0;
                }
                qInfo() << "PLANK replacement worker has no app to resume; "
                           "launching a fresh Desktop stream";
                startApp();
            }
            else {
                const bool generationBinding =
                        (m_Computer->plankFeatureFlags &
                         NvOutputTopology::TopologyGenerationFeature) != 0;
                if (!m_Computer->plankAuthentication || !generationBinding ||
                        e.getStatusCode() != 409) {
                    throw;
                }

                const NvOutputTopology topology = http->getOutputTopology();
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->outputTopology = topology;
                }
                if (m_ComputerManager != nullptr) {
                    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
                }
                if (!configurePlankLaunchGeometry()) {
                    return false;
                }
                qInfo() << "PLANK refreshed stale topology and launch geometry; retrying launch:"
                        << topology.generation << m_StreamConfig.width
                        << m_StreamConfig.height;
                startApp();
            }
        }

        // Record the successful launch immediately. If low-level transport
        // setup fails afterward, the next bounded attempt must resume this
        // app instead of issuing a second launch request.
        if (m_Computer->plankAuthentication) {
            m_PlankWorkerInstance = (m_Computer->plankFeatureFlags & NvOutputTopology::WorkerInstanceFeature) ?
                        http->workerInstance() : QString();
            m_PlankHostCertificateSha256 = plankTransportCertificateSha256;
            QWriteLocker lock(&m_Computer->lock);
            m_Computer->currentGameId = m_App.id;
        }

        if (m_Computer->plankAuthentication) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken.fill(QChar('\0'));
                m_Computer->sessionToken.clear();
                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
            }
            if (m_ComputerManager != nullptr) {
                m_ComputerManager->clientSideAttributeUpdated(m_Computer);
            }
            qInfo() << "PLANK authentication token consumed after launch";
        }
    } catch (const GfeHttpResponseException& e) {
        if (reconnecting && PlankReconnectPolicy::terminalStatus(e.getStatusCode(), false)) {
            // Operator consent cannot recover through automatic reauthentication.
            m_ReconnectCancelled.store(true);
            emit displayLaunchError(e.toQString());
        }
        m_WaitingForSessionCleanup.store(false);
        emit sessionCleanupWaitChanged(false, QString());
        if (reconnecting && e.getStatusCode() == 401) {
            QWriteLocker lock(&m_Computer->lock);
            m_Computer->sessionToken.fill(QChar('\0'));
            m_Computer->sessionToken.clear();
            m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
        }
        if (!reconnecting) {
            emit displayLaunchError(tr("Host returned error: %1").arg(e.toQString()));
        } else {
            qWarning() << "PLANK reconnect launch failed:" << e.toQString();
        }
        return false;
    } catch (const QtNetworkReplyException& e) {
        if (reconnecting && e.getError() == QNetworkReply::SslHandshakeFailedError) {
            m_ReconnectCancelled.store(true);
            emit displayLaunchError(e.toQString());
        }
        m_WaitingForSessionCleanup.store(false);
        emit sessionCleanupWaitChanged(false, QString());
        if (!reconnecting) {
            emit displayLaunchError(e.toQString());
        } else {
            qWarning() << "PLANK reconnect transport setup failed:"
                       << e.toQString();
        }
        return false;
    }

#ifdef PLANK_TRANSPORT
    LiSetPlankNativeControlSender(nullptr, nullptr);
    LiSetPlankNativeInputSender(nullptr, nullptr);
#endif
    if (!startPlankTransportDataPlane(plankTransportPort,
                                 plankTransportCertificateSha256,
                                 plankTransportToken,
                                 quicUdpPayloadMtu)) {
        plankTransportToken.fill(QChar('\0'));
        if (!reconnecting) {
            emit displayLaunchError(
                        tr("The experimental PLANK data plane could not be established."));
        }
        return false;
    }
    plankTransportToken.fill(QChar('\0'));

    QString nativeNegotiationError;
    const bool negotiated = macCapture ?
                LiSetPlankNativeSessionConfiguration(&macLaunch.configuration) == 0 :
                negotiatePlankTransportSession(plankTransportPort, nativeNegotiationError);
    if (!negotiated) {
        if (macCapture) nativeNegotiationError = tr("The Mac returned an invalid native media configuration.");
        stopPlankTransportDataPlane();
        if (!reconnecting) {
            emit displayLaunchError(nativeNegotiationError);
        }
        else {
            qWarning() << "PLANK native session negotiation failed:"
                       << nativeNegotiationError;
        }
        return false;
    }

    QByteArray hostnameStr = m_Computer->activeAddress.address().toLatin1();
    QByteArray siAppVersion = m_Computer->appVersion.toLatin1();

    SERVER_INFORMATION hostInfo;
    LiInitializeServerInformation(&hostInfo);
    hostInfo.address = hostnameStr.data();
    hostInfo.serverInfoAppVersion = siAppVersion.data();
    hostInfo.serverCodecModeSupport = m_Computer->serverCodecModeSupport;

    // moonlight-common-c fills missing callbacks in the caller-owned table.
    // Restore the pull/push decoder contract before every reuse of this
    // Session for a PLANK desktop handoff.
    m_VideoCallbacks.submitDecodeUnit =
            (m_VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) ?
                nullptr : drSubmitDecodeUnit;

    int err = LiStartConnection(&hostInfo, &m_StreamConfig, &k_ConnCallbacks,
                                &m_VideoCallbacks, &m_AudioCallbacks,
                                NULL, 0, NULL, 0);
    if (err != 0) {
        stopPlankTransportDataPlane();
        // We already displayed an error dialog in the stage failure
        // listener.
        return false;
    }

#ifdef PLANK_TRANSPORT
    {
        std::lock_guard<std::mutex> guard(m_MicrophoneMutex);
        if (m_MicrophoneNegotiated) {
            if (!reconnecting) m_MicrophoneRequested.store(m_Preferences->microphoneAutomatic);
            m_Microphone.reset(new PlankMicrophone(m_PlankTransportEndpoint,
                m_MicrophoneRequested, m_Preferences->microphoneAutomaticInput, m_MicrophoneSchema == 3));
        }
    }
    {
        std::lock_guard<std::mutex> guard(m_CameraMutex);
        m_CameraRequested.store(false);
        if (m_CameraNegotiated) m_Camera.reset(new PlankCamera(m_PlankTransportEndpoint,
            m_CameraRequested, m_Preferences->cameraDevice));
    }
    startPlankTransportMediaReceivers();
#endif

    if (!macCapture && (LiGetHostFeatureFlags() & LI_FF_LOCAL_CURSOR) == 0) {
        qCritical("Host does not advertise required PLANK local cursor transport");
        stopPlankTransportMediaReceivers();
        LiStopConnection();
        stopPlankTransportDataPlane();
        emit displayLaunchError(
                    tr("This workstation does not support the required PLANK local cursor protocol."));
        return false;
    }

    emit connectionStarted();
    return true;
}

void Session::cancelConnectionStart()
{
    m_ConnectionStartCancelled.store(true);
    m_ActiveSessionTakeoverDecision.store(-1);
}

void Session::respondToActiveSessionTakeover(bool takeOver)
{
    int expected = 0;
    if (m_ActiveSessionTakeoverDecision.compare_exchange_strong(
                expected, takeOver ? 1 : -1)) {
        qInfo() << "PLANK active-session takeover decision:"
                << (takeOver ? "take over" : "cancel");
    }
}

void Session::setPlankReconnectStatus(const char* text, bool warning)
{
    emit qtReconnectStatusChanged(QString::fromUtf8(text), warning);
}

bool Session::beginPlankReconnect(
        PlankReconnectState& state)
{
    if (m_PlankUsername.isEmpty() ||
            m_PlankPassword.isEmpty()) {
        return false;
    }

    m_Reconnecting.store(true);
    m_ReconnectGreeterConfirmed.store(false);
    const bool openingDesktop = m_DesktopHandoffNoticeDeadline.exchange(0) > PlankClock::milliseconds();
    setPlankReconnectStatus(
                openingDesktop ? "Opening your desktop..." : "Waiting for workstation...", false);

    state = {};
    // The remote cursor transport stops with the host connection. Return
    // pointer ownership to the local compositor immediately so the user can
    // still see and navigate the whole client window while reconnecting. The
    // native reconnect prompt has its own Wayland surface, but cursor
    // visibility must not depend on entering that surface first.

    state.videoFormat = m_ActiveVideoFormat;
    state.videoWidth = m_ActiveVideoWidth;
    state.videoHeight = m_ActiveVideoHeight;
    state.videoFrameRate = m_ActiveVideoFrameRate;
    {
    std::lock_guard<std::mutex> decoderLock(m_DecoderLock);
    if (m_VideoDecoder != nullptr) {
        state.retainedRenderer = m_VideoDecoder->suspendForReconnect();
        if (!state.retainedRenderer) {
            delete m_VideoDecoder;
            m_VideoDecoder = nullptr;
        }
    }
    }
#ifdef PLANK_TRANSPORT
    stopPlankTransportMediaReceivers();
#endif
    LiStopConnection();
    stopPlankTransportDataPlane();

    return true;
}

bool Session::waitForPlankReconnectRequest(bool restartAuthenticationAfterWait)
{
    bool waited = false;
    while (!m_ReconnectCancelled.load() && !m_ConnectionStartCancelled.load()) {
        if (m_ReconnectPolicy.allowsRequest(PlankClock::milliseconds())) {
            // A password conversation may have expired while Ask was open.
            // Restart it after explicit consent, rather than submitting a stale
            // challenge response and treating its rejection as a bad password.
            return !(waited && restartAuthenticationAfterWait);
        }
        waited = true;
        QThread::msleep(50);
    }
    return false;
}

bool Session::runPlankReconnect()
{
    if (m_PlankUsername.isEmpty() ||
            m_PlankPassword.isEmpty()) {
        return false;
    }

    {
        QWriteLocker lock(&m_Computer->lock);
        m_Computer->sessionToken.fill(QChar('\0'));
        m_Computer->sessionToken.clear();
        m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
        m_Computer->currentGameId = 0;
    }
    for (int attempt = 1; waitForPlankReconnectRequest(); ++attempt) {
        bool authenticating = false;
        try {
            QString token;
            {
                QReadLocker lock(&m_Computer->lock);
                token = m_Computer->sessionToken;
            }
            NvHTTP http(m_Computer);
            http.setRequestGate([this](bool auth) { return waitForPlankReconnectRequest(auth); });
            if (token.isEmpty()) {
                authenticating = true;
                bool greeterConfirmed = false;
                token = http.authenticate(m_PlankUsername, m_PlankPassword, &greeterConfirmed,
                                          NvHTTP::AuthenticationIntent::Recovery);
                authenticating = false;
                {
                    QWriteLocker lock(&m_Computer->lock);
                    m_Computer->sessionToken = token;
                    m_Computer->sessionIdentityKey = http.hostIdentityKey();
                    m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                }
                if (greeterConfirmed &&
                        ((m_Computer->plankFeatureFlags & NvOutputTopology::AuthenticatedDesktopStageFeature) ||
                         m_Computer->plankFeatureFlags == NvOutputTopology::FixedCaptureFlags)) {
                    m_ReconnectGreeterConfirmed.store(true);
                }
            }

            NvOutputTopology topology;
            bool topologySupported;
            bool macDesktop;
            QString desktopMode;
            int desktopScale = 1;
            {
                QReadLocker lock(&m_Computer->lock);
                topologySupported = NvOutputTopology::supportsDescription(
                            m_Computer->plankTopologyVersion, m_Computer->plankFeatureFlags);
                macDesktop = m_Computer->plankFeatureFlags == NvOutputTopology::FixedCaptureFlags;
                desktopMode = m_Computer->plankVirtualMode1;
                if (macDesktop && m_Computer->plankHostLayout == NvOutputTopology::MatchClientHostLayout) {
                    QVector<NvClientDisplay> displays;
                    for (const auto& display : std::as_const(m_ClientDisplays)) {
                        displays.append({display.macMatchedBounds.isValid() ? display.macMatchedBounds : QRect(display.logicalBounds.x(), display.logicalBounds.y(),
                                               display.logicalBounds.width(), display.logicalBounds.height()), display.nativeSize, display.macBackingSize});
                    }
                    desktopMode = NvOutputTopology::resolveMacClientDisplayMode(displays, nullptr, &desktopScale);
                    if (desktopMode.isEmpty()) return false;
                }
            }
            if (topologySupported) {
                topology = macDesktop ? http.prepareMacDisplay(desktopMode,
                    StreamingPreferences::plankAppleEncodingMode(m_PlankVideoProfile), desktopScale) : http.getOutputTopology();
            }
            const QVector<NvApp> apps = http.getAppList();
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken = token;
                m_Computer->sessionIdentityKey = http.hostIdentityKey();
                m_Computer->authorizationState = NvComputer::AS_AUTHORIZED;
                if (topologySupported) {
                    m_Computer->outputTopology = topology;
                }
                m_Computer->updateAppList(apps);
            }

            if (waitForPlankReconnectRequest() && startConnectionAsync(true) &&
                    !m_ReconnectCancelled.load() && m_ReconnectPolicy.allowsRequest(PlankClock::milliseconds())) {
                qInfo("PLANK reconnect transport completed on attempt %d",
                            attempt);
                return true;
            }
        } catch (const MacSessionActiveException&) {
            m_CanReconnect.store(false);
            m_ReconnectCancelled.store(true);
            emit displayLaunchError(tr("Another client has an active PLANK session. Connect again to request takeover."));
        } catch (const GfeHttpResponseException& error) {
            qWarning() << "PLANK reconnect attempt" << attempt
                       << "failed:" << error.toQString();
            if (PlankReconnectPolicy::terminalStatus(error.getStatusCode(), authenticating)) {
                m_ReconnectCancelled.store(true);
                emit displayLaunchError(error.toQString());
            }
            if (error.getStatusCode() == 401) {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->sessionToken.fill(QChar('\0'));
                m_Computer->sessionToken.clear();
                m_Computer->authorizationState = NvComputer::AS_UNAUTHORIZED;
            }
        } catch (const QtNetworkReplyException& error) {
            if (error.getError() == QNetworkReply::SslHandshakeFailedError) {
                m_ReconnectCancelled.store(true);
                emit displayLaunchError(error.toQString());
            }
            qWarning() << "PLANK reconnect attempt" << attempt
                       << "was deferred or failed:" << error.toQString();
        }

#ifdef PLANK_TRANSPORT
        // If setup finished after the Ask deadline, discard that transport
        // before pausing. Do not keep a hidden connection alive (or accept a
        // dead one) while the operator is away from the unanswered prompt.
        stopPlankTransportMediaReceivers();
#endif
        LiStopConnection();
        stopPlankTransportDataPlane();
        if (!m_ReconnectCancelled.load()) {
            constexpr int RetryDelayMs = 1000;
            constexpr int CancellationPollMs = 50;
            for (int elapsedMs = 0;
                 elapsedMs < RetryDelayMs && !m_ReconnectCancelled.load();
                 elapsedMs += CancellationPollMs) {
                QThread::msleep(CancellationPollMs);
            }
        }
    }

    return false;
}

bool Session::finishPlankReconnect(
        bool success,
        const PlankReconnectState& state, bool* decoderNeedsReset)
{
    bool resumedRenderer = false;
    if (success) {
        const bool streamConfigurationUnchanged =
                state.videoFormat == m_ActiveVideoFormat &&
                state.videoWidth == m_ActiveVideoWidth &&
                state.videoHeight == m_ActiveVideoHeight &&
                state.videoFrameRate == m_ActiveVideoFrameRate;
        if (state.retainedRenderer && streamConfigurationUnchanged) {
            {
    std::lock_guard<std::mutex> decoderLock(m_DecoderLock);
            resumedRenderer = m_VideoDecoder != nullptr &&
                    m_VideoDecoder->resumeAfterReconnect();
            }
        }
        m_UnexpectedTermination = false;
    } else {
        m_UnexpectedTermination = true;
    }

    m_Reconnecting.store(false);
    m_ReconnectGreeterConfirmed.store(false);
    m_DesktopHandoffNoticeDeadline.store(0);
    setPlankReconnectStatus("", false);
    m_ReconnectRequested = false;

    if (!success) {
        return false;
    }

    if (!resumedRenderer) {
        if (decoderNeedsReset) *decoderNeedsReset = true;
        else requestDecoderReset();
    } else {
        LiRequestIdrFrame();
    }

    qInfo("PLANK reconnect completed (%s renderer)",
                resumedRenderer ? "retained" : "recreated");
    return true;
}

// At most one bounded, credential-free probe is outstanding. It owns its
// address/identity snapshots and never reads or changes Session state.

void Session::exec(QWindow* qtWindow)
{
#ifdef Q_OS_MACOS
    auto* application = static_cast<MacApplication*>(QCoreApplication::instance());
    if (!application->beginSession()) {
        emit readyForDeletion();
        return;
    }
    const auto exitConnection = connect(application, &MacApplication::exitRequested,
                                       this, [this] {
        m_ApplicationExitRequested.store(true);
        cancelConnectionStart();
        m_ReconnectCancelled.store(true);
        m_CanReconnect.store(false);
    });
    // sessionFinished precedes asynchronous transport cleanup. Only this
    // later signal releases application ownership. Queue onto Qt's thread;
    // the Session itself may already have been garbage-collected by QML.
    connect(this, &Session::readyForDeletion, application,
            [application, exitConnection] {
        QObject::disconnect(exitConnection);
        application->endSession();
    }, Qt::QueuedConnection);
#endif
    auto* runtime = new QtSessionRuntime(this);
    runtime->start(qtWindow);
}
