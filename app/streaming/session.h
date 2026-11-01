#pragma once

#include <QtGlobal>

#include <QSemaphore>
#include <QSize>
#include <QStringList>
#include <QVector>
#include <QWindow>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include "plankcallbacktarget.h"
#include <thread>

#include <Limelight.h>
#include <opus_multistream.h>
#include "settings/streamingpreferences.h"
#include "video/decoder.h"
#include "backend/nvcomputer.h"
#include "backend/nvapp.h"
#include "audio/renderers/renderer.h"
#include "audio/microphone.h"
#include "camera/camera.h"
#include "videopacketlosswindow.h"
#include "plankreconnectpolicy.h"

class ComputerManager;
class MacClipboardSync;
class QtSessionRuntime;
class QtSessionInput;
#ifdef PLANK_TRANSPORT
struct PlankTransportNativeEndpoint;
#endif

class SupportedVideoFormatList : public QList<int>
{
public:
    operator int() const
    {
        int value = 0;

        for (const int & v : *this) {
            value |= v;
        }

        return value;
    }

    void
    removeByMask(int mask)
    {
        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                this->removeAt(i);
            }
            else {
                i++;
            }
        }
    }

    void
    deprioritizeByMask(int mask)
    {
        QList<int> deprioritizedList;

        int i = 0;
        while (i < this->length()) {
            if (this->value(i) & mask) {
                deprioritizedList.append(this->takeAt(i));
            }
            else {
                i++;
            }
        }

        this->append(std::move(deprioritizedList));
    }

    int maskByServerCodecModes(int serverCodecModes)
    {
        int mask = 0;

        // Feature-only flags don't map to a video format.
        serverCodecModes &= ~SCM_IDENTITY_GBR_444;

        const QMap<int, int> mapping = {
            {SCM_H264, VIDEO_FORMAT_H264},
            {SCM_H264_HIGH8_422, VIDEO_FORMAT_H264_HIGH8_422},
            {SCM_H264_HIGH8_444, VIDEO_FORMAT_H264_HIGH8_444},
            {SCM_H264_HIGH10_422, VIDEO_FORMAT_H264_HIGH10_422},
            {SCM_H264_HIGH10_444, VIDEO_FORMAT_H264_HIGH10_444},
            {SCM_HEVC, VIDEO_FORMAT_H265},
            {SCM_HEVC_MAIN10, VIDEO_FORMAT_H265_MAIN10},
            {SCM_HEVC_REXT8_444, VIDEO_FORMAT_H265_REXT8_444},
            {SCM_HEVC_REXT10_444, VIDEO_FORMAT_H265_REXT10_444},
            {SCM_AV1_MAIN8, VIDEO_FORMAT_AV1_MAIN8},
            {SCM_AV1_MAIN10, VIDEO_FORMAT_AV1_MAIN10},
            {SCM_AV1_HIGH8_444, VIDEO_FORMAT_AV1_HIGH8_444},
            {SCM_AV1_HIGH10_444, VIDEO_FORMAT_AV1_HIGH10_444},
        };

        for (QMap<int, int>::const_iterator it = mapping.cbegin(); it != mapping.cend(); ++it) {
            if (serverCodecModes & it.key()) {
                mask |= it.value();
                serverCodecModes &= ~it.key();
            }
        }

        // Make sure nobody forgets to update this for new SCM values
        Q_ASSERT(serverCodecModes == 0);

        int val = *this;
        return val & mask;
    }
};

class Session : public QObject
{
    Q_OBJECT

    friend class AsyncConnectionStartThread;
    friend class QtSessionRuntime;

public:
    explicit Session(NvComputer* computer, NvApp& app,
                     StreamingPreferences *preferences = nullptr,
                     ComputerManager *computerManager = nullptr);

    // NB: This may not get destroyed for a long time! Don't put any cleanup here.
    // QtSessionRuntime owns asynchronous teardown before readyForDeletion.
    virtual ~Session();

    Q_INVOKABLE void exec(QWindow* qtWindow);

    Q_INVOKABLE void cancelConnectionStart();

    Q_INVOKABLE void respondToActiveSessionTakeover(bool takeOver);

    static Session* get()
    {
        return s_ActiveSession;
    }

    // Worker-to-UI requests are queued to the Qt session owner.
    static void requestSessionQuit();
    static void requestDecoderReset();
    bool usesQtPresentation() const { return m_RendererFactory != nullptr; }

    void updateRenderedStats(float fps, float videoMbps)
    {
        m_CurrentRenderedFps.store(fps, std::memory_order_relaxed);
        m_CurrentVideoMbps.store(videoMbps, std::memory_order_relaxed);
    }

    VideoFecLossPercent currentVideoFecLoss() const
    {
        std::lock_guard<std::mutex> lock(m_VideoPacketLossSamplesLock);
        return m_CurrentVideoFecLoss;
    }

    std::uint32_t currentNetworkRttMs() const
    {
        return m_CurrentNetworkRttMs.load(std::memory_order_relaxed);
    }

signals:
    void qtStatisticsUpdated(QString text);
    void qtQuitRequested();
    void qtDecoderResetRequested();
    void qtReconnectRequested();
    void qtReconnectStatusChanged(QString text, bool warning);
    void stageStarting(QString stage);

    void stageFailed(QString stage, int errorCode, QString failingPorts);

    void connectionStarted();

    void sessionCleanupWaitChanged(bool waiting, QString text);

    void activeSessionTakeoverRequested(QString text);

    void displayLaunchError(QString text);

    void displayLaunchWarning(QString text);

    void sessionFinished();

    // Emitted after sessionFinished() when the session is ready to be destroyed
    void readyForDeletion();

private:
#ifdef Q_OS_MACOS
    std::atomic_bool m_ApplicationExitRequested{false};
#endif

    bool initialize(IFFmpegRendererFactory* presenter = nullptr);

    bool startConnectionAsync(bool reconnecting = false,
                              bool takeOverActiveSession = false);

    struct PlankReconnectState {
        bool retainedRenderer = false;
        int videoFormat = 0;
        int videoWidth = 0;
        int videoHeight = 0;
        int videoFrameRate = 0;
    };

    bool beginPlankReconnect(PlankReconnectState& state);
    void setPlankReconnectStatus(const char* text, bool warning);

    bool runPlankReconnect();
    bool waitForPlankReconnectRequest(bool restartAuthenticationAfterWait = false);

    bool finishPlankReconnect(bool success,
                             const PlankReconnectState& state, bool* decoderNeedsReset = nullptr);

    void clearPlankReconnectCredentials();

    bool startPlankTransportDataPlane(quint16 port,
                                 const QString& certificateSha256,
                                 const QString& token,
                                 quint16 quicUdpPayloadMtu);

    bool negotiatePlankTransportSession(quint16 sessionPort, QString& errorMessage);

    void stopPlankTransportDataPlane();

#ifdef PLANK_TRANSPORT
    void startPlankTransportMediaReceivers();
    void stopPlankTransportMediaReceivers();
    void plankTransportVideoReceiveLoop();
    void plankTransportAudioReceiveLoop();
    void plankTransportDataReceiveLoop();
    static int plankTransportNativeControlSender(void* context, uint32_t type,
                                            uint32_t value1, uint32_t value2);
    static int plankTransportNativeInputSender(void* context, uint8_t type,
                                          const unsigned char* payload,
                                          size_t payloadLength);
#endif
#ifdef Q_OS_MACOS
    void createClipboardSync();
    void startClipboardSync();
    void stopClipboardSync();
    bool clipboardSyncEnabled() const;
#endif

    bool validateLaunch();

    void emitLaunchWarning(QString text);

    bool populateDecoderProperties();

    IAudioRenderer* createAudioRenderer(const POPUS_MULTISTREAM_CONFIGURATION opusConfig);

    bool initializeAudioRenderer();

    bool testAudio(int audioConfiguration);

    int getAudioRendererCapabilities(int audioConfiguration);

    bool configurePlankHostLayout();

    QSize configurePlankDisplayMode();

    bool configurePlankLaunchGeometry();

    enum class DecoderAvailability {
        None,
        Software,
        Hardware
    };

    DecoderAvailability getDecoderAvailability(int videoFormat, int width, int height, int frameRate,
                                               bool enableIdentityGbr = false);

    static
    bool chooseDecoder(DecoderSelectionMode selectionMode,
                       int videoFormat, int width, int height,
                       int frameRate, bool enableVsync, bool testOnly,
                       IVideoDecoder*& chosenDecoder,
                       bool enableIdentityGbr = false,
                       DecoderCaptureSource captureSource =
                           DecoderCaptureSource::Nvfbc8Bit,
                       DecoderEncoderBackend encoderBackend =
                           DecoderEncoderBackend::SoftwareCuda,
                       IFFmpegRendererFactory* presenter = nullptr);

    bool isIdentityGbrEnabledForFormat(int videoFormat) const;
    DecoderCaptureSource decoderCaptureSource() const;
    DecoderEncoderBackend decoderEncoderBackend() const;

    static
    void clStageStarting(int stage);

    static
    void clStageFailed(int stage, int errorCode);

    static
    void clConnectionTerminated(int errorCode);

    static
    void clLogMessage(const char* format, ...);

    static
    void clConnectionStatusUpdate(int connectionStatus);

    static
    void clSetHdrMode(bool enabled);

    static
    void clRawHidControl(const unsigned char* data, unsigned int length);

    static
    void clVideoBitrateApplied(uint32_t requestedKbps, uint32_t appliedKbps, uint32_t peakKbps);

    static
    void clCursorChunk(const unsigned char* data, unsigned int length);

    static
    void clCursorPosition(const unsigned char* data, unsigned int length);

    static
    void updateVideoFecLoss(VideoFecLossPercent loss);

    static
    int arInit(int audioConfiguration,
               const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
               void* arContext, int arFlags);

    static
    void arCleanup();

    static
    void arDecodeAndPlaySample(char* sampleData, int sampleLength, int64_t presentationTimeUs);

    static
    int drSetup(int videoFormat, int width, int height, int frameRate, void*, int);

    static
    void drCleanup();

    static
    int drSubmitDecodeUnit(PDECODE_UNIT du);

    StreamingPreferences* m_Preferences;
    bool m_IsFullScreen;
    SupportedVideoFormatList m_SupportedVideoFormats; // Sorted in order of descending priority
    STREAM_CONFIGURATION m_StreamConfig;
    bool m_MacClipboardNegotiated = false;
    bool m_CameraNegotiated = false;
    std::atomic<bool> m_CameraRequested {false};
    std::mutex m_CameraMutex;
    std::unique_ptr<PlankCamera> m_Camera;
    bool m_MicrophoneNegotiated = false;
    unsigned m_MicrophoneSchema = 2;
    std::atomic<bool> m_MicrophoneRequested {false};
    std::mutex m_MicrophoneMutex;
    std::unique_ptr<PlankMicrophone> m_Microphone;
    DECODER_RENDERER_CALLBACKS m_VideoCallbacks;
    AUDIO_RENDERER_CALLBACKS m_AudioCallbacks;
    NvComputer* m_Computer;
    StreamingPreferences::PlankVideoProfile m_PlankVideoProfile;
    StreamingPreferences::PlankCaptureSource m_PlankCaptureSource;
#ifdef PLANK_TRANSPORT
    PlankTransportNativeEndpoint* m_PlankTransportEndpoint = nullptr;
    std::atomic_bool m_PlankTransportReceiversStopping {false};
    std::thread m_PlankTransportVideoThread;
    std::thread m_PlankTransportAudioThread;
    std::thread m_PlankTransportDataThread;
#endif
    int m_PlankBitrateKbps;
    ComputerManager* m_ComputerManager;
    NvApp m_App;
    IVideoDecoder* m_VideoDecoder;
    // Immutable during one active runtime, owned until all decoder workers exit.
    IFFmpegRendererFactory* m_RendererFactory = nullptr;
    std::mutex m_DecoderLock;
    bool m_AudioDisabled;
    bool m_AudioMuted;
    bool m_UnexpectedTermination;
    std::atomic_bool m_ReconnectRequested;
    std::atomic<quint64> m_DesktopHandoffNoticeDeadline {0};
    std::atomic_bool m_ReconnectGreeterConfirmed {false};
    std::atomic<quint64> m_LastPlankVideoReceived {0};
    QString m_PlankWorkerInstance;
    QString m_PlankHostCertificateSha256;
    std::atomic_bool m_Reconnecting;
    std::atomic_bool m_ReconnectCancelled;
    PlankReconnectPolicy m_ReconnectPolicy;
    std::atomic_bool m_CanReconnect;
    std::atomic_bool m_ConnectionStartCancelled;
    std::atomic_bool m_WaitingForSessionCleanup;
    std::atomic_bool m_WaitingForActiveSessionTakeoverDecision {false};
    std::atomic_int m_ActiveSessionTakeoverDecision {0};
    QString m_PlankUsername;
    QString m_PlankPassword;
    QString m_ResolvedScalingMode;
    QString m_ResolvedHostLayout;
    QStringList m_ResolvedVirtualModes;
    int m_ResolvedPrimaryOutput = -1;

    struct ClientDisplaySnapshot {
        int displayId = 0;
        QRect logicalBounds = {};
        QSize nativeSize;
        QSize macBackingSize;
        QRect macMatchedBounds;
        QRect canvasRect;
        bool primary = false;
    };
    QVector<ClientDisplaySnapshot> m_ClientDisplays;
    int m_TargetDisplayId = 0;
    bool m_MultiDisplayPresentationAvailable = false;
    bool m_UseMultiDisplayPresentation = false;
    PlankCallbackTarget<QtSessionInput> m_QtInputCallbacks;

    bool m_AsyncConnectionSuccess;
    int m_ActiveVideoFormat;
    int m_ActiveVideoWidth;
    int m_ActiveVideoHeight;
    int m_ActiveVideoFrameRate;

    OpusMSDecoder* m_OpusDecoder;
    IAudioRenderer* m_AudioRenderer;
    OPUS_MULTISTREAM_CONFIGURATION m_ActiveAudioConfig;
    OPUS_MULTISTREAM_CONFIGURATION m_OriginalAudioConfig;
    int m_AudioSampleCount;
    quint64 m_DropAudioEndTime;
    quint64 m_AudioMediaFramesReceived;
    bool m_AvSyncTelemetryEnabled;
    quint64 m_LastAudioTelemetryTime;

#ifdef Q_OS_MACOS
    std::unique_ptr<MacClipboardSync> m_ClipboardSync;
    std::atomic_bool m_QtClipboardFocused{false};
#endif
    std::atomic<float> m_CurrentRenderedFps;
    std::atomic<float> m_CurrentVideoMbps;
    VideoFecLossPercent m_CurrentVideoFecLoss;
    std::atomic<std::uint32_t> m_CurrentNetworkRttMs;
    mutable std::mutex m_VideoPacketLossSamplesLock;
    VideoPacketLossPeakWindow m_VideoPacketLossPeakWindow;
    VideoPacketLossPeakWindow m_VideoPacketLossAfterFecPeakWindow;
    std::atomic<int> m_ConfirmedBitrateRequestKbps {0};
    std::atomic<int> m_ConfirmedBitrateAppliedKbps {0};
    std::atomic<int> m_ConfirmedBitratePeakKbps {0};

    static CONNECTION_LISTENER_CALLBACKS k_ConnCallbacks;
    static Session* s_ActiveSession;
    static QSemaphore s_ActiveSessionSemaphore;
};
