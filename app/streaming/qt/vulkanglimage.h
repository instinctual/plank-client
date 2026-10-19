#pragma once

#include <QSize>
#include <QString>
#include <memory>
#include <libplacebo/vulkan.h>

class QOpenGLContext;

// One dedicated RGB10 image shared between Vulkan conversion and Qt's GL
// renderer. All methods run on the owning Qt render thread, with its GL context
// current. Neither API may sample/write while the other owns the image.
// Binary GPU semaphores transfer ownership; no per-frame CPU wait or readback.
class QtVulkanGlImage final
{
public:
    static bool supported(pl_vulkan vk, QString& error);
    static std::unique_ptr<QtVulkanGlImage> create(
        pl_vulkan vk, std::shared_ptr<void> deviceLifetime, QSize size, QString& error);
    ~QtVulkanGlImage();

    QtVulkanGlImage(const QtVulkanGlImage&) = delete;
    QtVulkanGlImage& operator=(const QtVulkanGlImage&) = delete;

    // First write needs no wait. Later writes consume the GL return semaphore.
    // Must be followed by acquireForGl(), even if conversion fails, to return
    // ownership before another write. Retirement can destroy in either state.
    pl_tex beginWrite(QString& error);
    bool acquireForGl(QString& error);
    // The returned texture remains valid for repeated draws until releaseGl().
    unsigned int glTexture() const;
    bool releaseGl(QString& error);
    QSize size() const { return m_Size; }

private:
    QtVulkanGlImage(pl_vulkan vk, std::shared_ptr<void> lifetime, QSize size);
    bool initialize(QString& error);
    bool current() const;
    struct Api;
    std::unique_ptr<Api> m_Api;
    std::shared_ptr<void> m_DeviceLifetime;
    pl_vulkan m_Vulkan;
    QOpenGLContext* m_Context;
    QSize m_Size;
    VkImage m_Image = VK_NULL_HANDLE;
    VkDeviceMemory m_Memory = VK_NULL_HANDLE;
    VkSemaphore m_Ready = VK_NULL_HANDLE;
    VkSemaphore m_Returned = VK_NULL_HANDLE;
    pl_tex m_Texture = nullptr;
    unsigned int m_GlMemory = 0, m_GlTexture = 0, m_GlReady = 0, m_GlReturned = 0;
    enum class Owner { Initial, Vulkan, Gl, Returned, Failed };
    Owner m_Owner = Owner::Initial;
    bool m_Held = true;
};
