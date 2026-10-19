#include "softwarevideodevice.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <cstring>

struct QtSoftwareVideoDevice::Device {
    pl_vulkan vk = nullptr;
    ~Device() { pl_vulkan_destroy(&vk); }
};

QtSoftwareVideoDevice::QtSoftwareVideoDevice(std::shared_ptr<Device> device)
    : m_Device(std::move(device)),
      m_Converter(std::make_unique<QtSoftwareVideoConverter>(m_Device->vk->gpu, m_Device)) {}

QtSoftwareVideoDevice::~QtSoftwareVideoDevice() = default;

QtSoftwareVideoDevice::Ptr QtSoftwareVideoDevice::createForCurrentContext(QString& error)
{
    error.clear();
    auto* context = QOpenGLContext::currentContext();
    if (!context || !context->hasExtension("GL_EXT_memory_object_fd") ||
            !context->hasExtension("GL_EXT_semaphore_fd")) {
        error = QStringLiteral("The Qt software video path needs GPU memory and semaphore sharing");
        return {};
    }
    // Select the renderer's exact physical GPU, not libplacebo's preferred
    // discrete GPU on a multi-GPU machine. Ambiguous GL devices fail closed.
    using GetIndexedBytes = void (*)(GLenum, GLuint, GLubyte*);
    const auto getBytes = reinterpret_cast<GetIndexedBytes>(context->getProcAddress("glGetUnsignedBytei_vEXT"));
    GLint devices = 0;
    context->extraFunctions()->glGetIntegerv(0x9596 /* GL_NUM_DEVICE_UUIDS_EXT */, &devices);
    if (!getBytes || devices != 1) {
        error = QStringLiteral("Qt could not identify a single presentation GPU"); return {};
    }
    auto params = pl_vulkan_default_params;
    getBytes(0x9597 /* GL_DEVICE_UUID_EXT */, 0, params.device_uuid);
    const unsigned char zero[16]{};
    if (context->extraFunctions()->glGetError() != GL_NO_ERROR ||
            !std::memcmp(params.device_uuid, zero, sizeof(zero))) {
        error = QStringLiteral("Qt could not read the presentation GPU identity"); return {};
    }
    // Exclusive external images have one Vulkan ownership queue. These
    // settings affect this experimental device only, not the existing renderer.
    params.async_compute = params.async_transfer = false;
    const char* extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
    params.extensions = extensions; params.num_extensions = 2;
    auto device = std::make_shared<Device>();
    device->vk = pl_vulkan_create(nullptr, &params);
    if (!device->vk) {
        error = QStringLiteral("Could not create the Qt software video GPU device"); return {};
    }
    if (!PlVkFramePool::supported(device->vk->gpu) ||
            !QtSoftwareVideoConverter::outputFormat(device->vk->gpu)) {
        error = QStringLiteral("The Qt GPU cannot import decoder memory or preserve RGB10 precision"); return {};
    }
    if (!QtVulkanGlImage::supported(device->vk, error)) return {};
    return Ptr(new QtSoftwareVideoDevice(std::move(device)));
}

int QtSoftwareVideoDevice::getBuffer(AVCodecContext* context, AVFrame* frame, int flags)
{
    return m_Converter->framePool().getBuffer(context, frame, flags);
}

std::unique_ptr<QtVulkanGlImage> QtSoftwareVideoDevice::createImage(QSize size, QString& error)
{
    return QtVulkanGlImage::create(m_Device->vk, m_Device, size, error);
}

bool QtSoftwareVideoDevice::convert(const AVFrame* frame, AVPixelFormat format,
                                     QtVulkanGlImage& image, QString& error)
{
    std::lock_guard<std::mutex> lock(m_ConversionMutex);
    const auto output = image.beginWrite(error);
    if (!output) return false;
    const bool converted = m_Converter->convert(frame, format, output, error);
    // Even a rejected frame returns ownership. It must not leave the next
    // frame waiting for an unsignalled semaphore or reusing a Vulkan-held image.
    QString transferError;
    const bool transferred = image.acquireForGl(transferError);
    if (!transferred) error = transferError;
    return converted && transferred;
}
