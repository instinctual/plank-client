#pragma once

#include "../video/ffmpeg-renderers/renderer.h"
#include "softwarevideodevice.h"
#include "decodedvideodelivery.h"
#include <functional>

// Reuses FFmpegVideoDecoder's exact-profile fixtures, hardware-first selection,
// worker, transport and recovery. Only the presentation backend changes.
class QtDecodedVideoRendererFactory final : public IFFmpegRendererFactory
{
public:
    using OpenDelivery = std::function<QtDecodedVideoDelivery()>;
    QtDecodedVideoRendererFactory(QtSoftwareVideoDevice::Ptr softwareDevice,
                                  QString vaapiRenderNode, OpenDelivery openDelivery);
    IFFmpegRenderer* createRenderer(const AVCodecHWConfig* hardware) override;

private:
    QtSoftwareVideoDevice::Ptr m_SoftwareDevice;
    const QString m_VaapiRenderNode;
    const OpenDelivery m_OpenDelivery;
};
