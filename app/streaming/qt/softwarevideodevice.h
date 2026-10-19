#pragma once

#include "softwarevideoconverter.h"
#include "vulkanglimage.h"
#include <mutex>

// Shared by the decoder and all output renderers. Created with a Qt GL context
// current, then published to the decoder. No surface, SDL video initialization
// or native event dispatch is needed. AVBuffer leases retain only the Vulkan
// lifetime, not this object (avoiding a pool/device reference cycle).
class QtSoftwareVideoDevice final
{
public:
    using Ptr = std::shared_ptr<QtSoftwareVideoDevice>;
    static Ptr createForCurrentContext(QString& error);
    ~QtSoftwareVideoDevice();

    int getBuffer(AVCodecContext* context, AVFrame* frame, int flags);
    std::unique_ptr<QtVulkanGlImage> createImage(QSize size, QString& error);
    bool convert(const AVFrame* frame, AVPixelFormat format, QtVulkanGlImage& image, QString& error);

private:
    struct Device;
    explicit QtSoftwareVideoDevice(std::shared_ptr<Device> device);
    std::shared_ptr<Device> m_Device;
    std::unique_ptr<QtSoftwareVideoConverter> m_Converter;
    // A converter has reusable plane textures/dispatch state. Different Qt
    // output render threads must not mutate that state concurrently. The lock
    // covers GPU submission only; never presentation, GUI calls or a GPU wait.
    std::mutex m_ConversionMutex;
};
