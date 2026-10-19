#pragma once

#include <cstdint>

#include <QSize>
#include <Limelight.h>
#include <SDL3/SDL.h>

#include "../plankpresentation.h"

class IFFmpegRendererFactory;

#define SDL_CODE_FRAME_READY 0

#define MAX_SLICES 4

enum class DecoderSelectionMode
{
    PreferExactHardwareThenSoftware,
    ExactHardwareOnly,
};

enum class DecoderCaptureSource
{
    Nvfbc8Bit,
    NativeX11_10Bit,
    ScreenCaptureKit,
};

enum class DecoderEncoderBackend
{
    SoftwareCuda,
    NvencDirect,
    VideoToolbox,
};

typedef struct _VIDEO_STATS {
    uint64_t receivedVideoBytes;
    uint32_t receivedFrames;
    uint32_t decodedFrames;
    uint32_t renderedFrames;
    uint32_t totalFrames;
    uint32_t networkDroppedFrames;
    uint32_t pacerDroppedFrames;
    uint32_t pacingQueueDroppedFrames;
    uint32_t renderQueueDroppedFrames;
    uint32_t queueOverflowDroppedFrames;
    uint16_t minHostProcessingLatency;
    uint16_t maxHostProcessingLatency;
    uint32_t totalHostProcessingLatency;
    uint32_t framesWithHostProcessingLatency;
    uint32_t totalReassemblyTime;
    uint32_t totalDecodeTime;
    uint32_t totalPacerTime;
    uint32_t totalRenderTime;
    uint32_t lastRtt;
    float totalFps;
    float receivedVideoMbps;
    float receivedFps;
    float decodedFps;
    float renderedFps;
    uint64_t measurementStartTimestamp;
} VIDEO_STATS, *PVIDEO_STATS;

typedef struct _DECODER_PARAMETERS {
    SDL_Window* window;
    DecoderSelectionMode selectionMode;
    DecoderCaptureSource captureSource;
    DecoderEncoderBackend encoderBackend;

    int videoFormat;
    int width;
    int height;
    int frameRate;
    bool enableVsync;
    bool enableFramePacing;
    bool enableIdentityGbr;
    bool testOnly;
    const PlankPresentationLayout* presentationLayout;
    // Optional UI-owned presenter. Null preserves the established SDL policy.
    // The factory outlives decoder startup, reconnect and destruction.
    IFFmpegRendererFactory* rendererFactory;
} DECODER_PARAMETERS, *PDECODER_PARAMETERS;

#define WINDOW_STATE_CHANGE_SIZE 0x01
#define WINDOW_STATE_CHANGE_DISPLAY 0x02

typedef struct _WINDOW_STATE_CHANGE_INFO {
    SDL_Window* window;
    uint32_t stateChangeFlags;

    // Populated if WINDOW_STATE_CHANGE_SIZE is set
    int width;
    int height;

    // Populated if WINDOW_STATE_CHANGE_DISPLAY is set
    SDL_DisplayID displayId;
} WINDOW_STATE_CHANGE_INFO, *PWINDOW_STATE_CHANGE_INFO;

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() {}
    virtual bool initialize(PDECODER_PARAMETERS params) = 0;
    virtual bool isHardwareAccelerated() = 0;
    virtual bool isAlwaysFullScreen() = 0;
    virtual bool isHdrSupported() = 0;
    virtual int getDecoderCapabilities() = 0;
    virtual int getDecoderColorspace() = 0;
    virtual int getDecoderColorRange() = 0;
    virtual QSize getDecoderMaxResolution() = 0;
    virtual int submitDecodeUnit(PDECODE_UNIT du) = 0;
    virtual void renderFrameOnMainThread() = 0;
    virtual void setHdrMode(bool enabled) = 0;
    virtual bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) = 0;

    // PLANK may replace the host media worker when GDM hands the
    // display to an authenticated desktop. Decoders that support it can pause
    // their transport-facing thread while retaining the renderer and its last
    // presented frame, keeping the native stream window continuously mapped.
    virtual bool suspendForReconnect() { return false; }
    virtual bool resumeAfterReconnect() { return false; }
};
