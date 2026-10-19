#include "vulkanglimage.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <array>
#include <cstring>
#include <unistd.h>

namespace {
constexpr GLenum HandleFd = 0x9586, DedicatedMemory = 0x9581;
constexpr GLenum GeneralLayout = 0x958D, Tiling = 0x9580, OptimalTiling = 0x9584;
constexpr GLenum DriverUuid = 0x9598, DeviceUuid = 0x9597, NumDeviceUuids = 0x9596;
constexpr VkFormat ImageFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
constexpr VkImageUsageFlags ImageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
template<typename T> T glProc(const char* name)
{
    return reinterpret_cast<T>(QOpenGLContext::currentContext()->getProcAddress(name));
}
}

struct QtVulkanGlImage::Api {
    void (*createMemory)(GLsizei, GLuint*) = glProc<decltype(createMemory)>("glCreateMemoryObjectsEXT");
    void (*deleteMemory)(GLsizei, const GLuint*) = glProc<decltype(deleteMemory)>("glDeleteMemoryObjectsEXT");
    void (*memoryParameter)(GLuint, GLenum, const GLint*) = glProc<decltype(memoryParameter)>("glMemoryObjectParameterivEXT");
    void (*importMemory)(GLuint, GLuint64, GLenum, GLint) = glProc<decltype(importMemory)>("glImportMemoryFdEXT");
    void (*textureStorage)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64) =
        glProc<decltype(textureStorage)>("glTexStorageMem2DEXT");
    void (*genSemaphores)(GLsizei, GLuint*) = glProc<decltype(genSemaphores)>("glGenSemaphoresEXT");
    void (*deleteSemaphores)(GLsizei, const GLuint*) = glProc<decltype(deleteSemaphores)>("glDeleteSemaphoresEXT");
    void (*importSemaphore)(GLuint, GLenum, GLint) = glProc<decltype(importSemaphore)>("glImportSemaphoreFdEXT");
    void (*waitSemaphore)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) =
        glProc<decltype(waitSemaphore)>("glWaitSemaphoreEXT");
    void (*signalSemaphore)(GLuint, GLuint, const GLuint*, GLuint, const GLuint*, const GLenum*) =
        glProc<decltype(signalSemaphore)>("glSignalSemaphoreEXT");
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;
    bool valid() const {
        return createMemory && deleteMemory && memoryParameter && importMemory && textureStorage &&
               genSemaphores && deleteSemaphores && importSemaphore && waitSemaphore && signalSemaphore &&
               getMemoryFd && getSemaphoreFd;
    }
};

bool QtVulkanGlImage::supported(pl_vulkan vk, QString& error)
{
    error.clear();
    auto* context = QOpenGLContext::currentContext();
    if (!vk || !context || !context->hasExtension("GL_EXT_memory_object_fd") ||
            !context->hasExtension("GL_EXT_semaphore_fd")) {
        error = QStringLiteral("Qt software video needs GL external-memory and semaphore FD support");
        return false;
    }
    // Opaque FD handles are not a cross-driver interchange format. Compare
    // actual identities, not vendor names, render-node numbering or PCI hints.
    auto getBytes = glProc<void (*)(GLenum, GLubyte*)>("glGetUnsignedBytevEXT");
    auto getIndexed = glProc<void (*)(GLenum, GLuint, GLubyte*)>("glGetUnsignedBytei_vEXT");
    if (!getBytes || !getIndexed) {
        error = QStringLiteral("Qt cannot verify the GL/Vulkan device identity");
        return false;
    }
    VkPhysicalDeviceIDProperties id{}; id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{}; properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id;
    vkGetPhysicalDeviceProperties2(vk->phys_device, &properties);
    std::array<GLubyte, VK_UUID_SIZE> driver{}, device{};
    getBytes(DriverUuid, driver.data());
    GLint devices = 0;
    auto* gl = context->extraFunctions();
    gl->glGetIntegerv(NumDeviceUuids, &devices);
    bool match = false;
    if (devices > 0 && devices <= 16 && !std::memcmp(driver.data(), id.driverUUID, VK_UUID_SIZE)) {
        for (int i = 0; i < devices; ++i) {
            getIndexed(DeviceUuid, GLuint(i), device.data());
            match |= !std::memcmp(device.data(), id.deviceUUID, VK_UUID_SIZE);
        }
    }
    if (!match || gl->glGetError() != GL_NO_ERROR) {
        error = QStringLiteral("Qt GL and Vulkan must use the same device and compatible driver");
        return false;
    }
    VkPhysicalDeviceExternalImageFormatInfo external{}; external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkPhysicalDeviceImageFormatInfo2 info{}; info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    info.pNext = &external; info.format = ImageFormat; info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = ImageUsage;
    VkExternalImageFormatProperties extProps{}; extProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 props{}; props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    props.pNext = &extProps;
    VkPhysicalDeviceExternalSemaphoreInfo semInfo{}; semInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
    semInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalSemaphoreProperties semProps{}; semProps.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
    vkGetPhysicalDeviceExternalSemaphoreProperties(vk->phys_device, &semInfo, &semProps);
    if (vkGetPhysicalDeviceImageFormatProperties2(vk->phys_device, &info, &props) != VK_SUCCESS ||
            !(extProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ||
            !(semProps.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) {
        error = QStringLiteral("Vulkan cannot export exact RGB10 images and synchronization to Qt");
        return false;
    }
    return true;
}

QtVulkanGlImage::QtVulkanGlImage(pl_vulkan vk, std::shared_ptr<void> lifetime, QSize size)
    : m_DeviceLifetime(std::move(lifetime)), m_Vulkan(vk),
      m_Context(QOpenGLContext::currentContext()), m_Size(size) {}

std::unique_ptr<QtVulkanGlImage> QtVulkanGlImage::create(
    pl_vulkan vk, std::shared_ptr<void> lifetime, QSize size, QString& error)
{
    if (!supported(vk, error)) return {};
    auto image = std::unique_ptr<QtVulkanGlImage>(new QtVulkanGlImage(vk, std::move(lifetime), size));
    if (!image->initialize(error)) return {};
    return image;
}

bool QtVulkanGlImage::initialize(QString& error)
{
    auto fail = [&](const char* stage) {
        error = QStringLiteral("Qt Vulkan/GL image setup failed: %1").arg(QString::fromLatin1(stage));
        return false;
    };
    if (m_Size.width() <= 0 || m_Size.height() <= 0 || m_Size.width() > 16384 || m_Size.height() > 16384)
        return fail("dimensions");
    // A single queue family is required for this exclusive image. Device
    // creation for this adapter disables asynchronous compute/transfer queues.
    if ((m_Vulkan->queue_compute.count && m_Vulkan->queue_compute.index != m_Vulkan->queue_graphics.index) ||
            (m_Vulkan->queue_transfer.count && m_Vulkan->queue_transfer.index != m_Vulkan->queue_graphics.index))
        return fail("exclusive queue family");
    m_Api = std::make_unique<Api>();
    m_Api->getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(m_Vulkan->device, "vkGetMemoryFdKHR"));
    m_Api->getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkGetDeviceProcAddr(m_Vulkan->device, "vkGetSemaphoreFdKHR"));
    if (!m_Api->valid()) return fail("entry points");
    const auto device = m_Vulkan->device;
    VkExternalMemoryImageCreateInfo external{}; external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkImageCreateInfo image{}; image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.pNext = &external; image.imageType = VK_IMAGE_TYPE_2D; image.format = ImageFormat;
    image.extent = {uint32_t(m_Size.width()), uint32_t(m_Size.height()), 1};
    image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = ImageUsage;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &image, nullptr, &m_Image) != VK_SUCCESS) return fail("image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, m_Image, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_Vulkan->phys_device, &memoryProperties);
    uint32_t type = memoryProperties.memoryTypeCount;
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        if ((requirements.memoryTypeBits & (1U << i)) &&
                (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i; break;
        }
    if (type == memoryProperties.memoryTypeCount) return fail("device-local memory");
    VkMemoryDedicatedAllocateInfo dedicated{}; dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = m_Image;
    VkExportMemoryAllocateInfo exporting{}; exporting.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exporting.pNext = &dedicated; exporting.handleTypes = external.handleTypes;
    VkMemoryAllocateInfo allocate{}; allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext = &exporting; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocate, nullptr, &m_Memory) != VK_SUCCESS ||
            vkBindImageMemory(device, m_Image, m_Memory, 0) != VK_SUCCESS) return fail("image memory");
    VkMemoryGetFdInfoKHR memoryFd{}; memoryFd.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    memoryFd.memory = m_Memory; memoryFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (m_Api->getMemoryFd(device, &memoryFd, &fd) != VK_SUCCESS || fd < 0) return fail("memory export");
    auto* gl = m_Context->extraFunctions();
    m_Api->createMemory(1, &m_GlMemory);
    const GLint yes = GL_TRUE;
    m_Api->memoryParameter(m_GlMemory, DedicatedMemory, &yes);
    m_Api->importMemory(m_GlMemory, requirements.size, HandleFd, fd);
    if (gl->glGetError() != GL_NO_ERROR) { close(fd); return fail("GL memory import"); }
    // Successful FD imports transfer descriptor ownership to GL.
    gl->glGenTextures(1, &m_GlTexture);
    gl->glBindTexture(GL_TEXTURE_2D, m_GlTexture);
    gl->glTexParameteri(GL_TEXTURE_2D, Tiling, OptimalTiling);
    m_Api->textureStorage(GL_TEXTURE_2D, 1, GL_RGB10_A2, m_Size.width(), m_Size.height(), m_GlMemory, 0);
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    if (gl->glGetError() != GL_NO_ERROR) return fail("GL RGB10 storage");
    auto semaphore = [&](VkSemaphore& vkSem, GLuint& glSem) {
        VkExportSemaphoreCreateInfo exportingSem{}; exportingSem.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
        exportingSem.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo create{}; create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO; create.pNext = &exportingSem;
        if (vkCreateSemaphore(device, &create, nullptr, &vkSem) != VK_SUCCESS) return false;
        VkSemaphoreGetFdInfoKHR get{}; get.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
        get.semaphore = vkSem; get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        int semFd = -1;
        if (m_Api->getSemaphoreFd(device, &get, &semFd) != VK_SUCCESS || semFd < 0) return false;
        m_Api->genSemaphores(1, &glSem);
        m_Api->importSemaphore(glSem, HandleFd, semFd);
        if (gl->glGetError() == GL_NO_ERROR) return true;
        close(semFd); return false;
    };
    if (!semaphore(m_Ready, m_GlReady) || !semaphore(m_Returned, m_GlReturned)) return fail("semaphores");
    pl_vulkan_wrap_params wrap{};
    wrap.image = m_Image; wrap.width = m_Size.width(); wrap.height = m_Size.height();
    wrap.format = ImageFormat; wrap.usage = ImageUsage;
    m_Texture = pl_vulkan_wrap(m_Vulkan->gpu, &wrap);
    if (!m_Texture) return fail("libplacebo image wrapper");
    return true;
}

bool QtVulkanGlImage::current() const { return QOpenGLContext::currentContext() == m_Context; }

pl_tex QtVulkanGlImage::beginWrite(QString& error)
{
    error.clear();
    if (!current() || (m_Owner != Owner::Initial && m_Owner != Owner::Returned)) {
        error = QStringLiteral("Qt GPU image is not available for Vulkan writing"); return nullptr;
    }
    pl_vulkan_release_params release{};
    release.tex = m_Texture;
    release.layout = m_Owner == Owner::Initial ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    release.qf = m_Owner == Owner::Initial ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL;
    if (m_Owner == Owner::Returned) release.semaphore.sem = m_Returned;
    pl_vulkan_release_ex(m_Vulkan->gpu, &release);
    m_Held = false; m_Owner = Owner::Vulkan;
    return m_Texture;
}

bool QtVulkanGlImage::acquireForGl(QString& error)
{
    error.clear();
    if (!current() || m_Owner != Owner::Vulkan) {
        error = QStringLiteral("Qt GPU image cannot be acquired before Vulkan submission"); return false;
    }
    pl_vulkan_hold_params hold{};
    hold.tex = m_Texture; hold.layout = VK_IMAGE_LAYOUT_GENERAL; hold.qf = VK_QUEUE_FAMILY_EXTERNAL;
    hold.semaphore.sem = m_Ready;
    if (!pl_vulkan_hold_ex(m_Vulkan->gpu, &hold)) {
        m_Owner = Owner::Failed;
        error = QStringLiteral("Qt Vulkan image ownership transfer failed"); return false;
    }
    m_Held = true;
    pl_gpu_flush(m_Vulkan->gpu);
    const GLenum layout = GeneralLayout;
    m_Api->waitSemaphore(m_GlReady, 0, nullptr, 1, &m_GlTexture, &layout);
    if (m_Context->extraFunctions()->glGetError() != GL_NO_ERROR) {
        m_Owner = Owner::Failed;
        error = QStringLiteral("Qt GL image ownership wait failed"); return false;
    }
    m_Owner = Owner::Gl;
    return true;
}

unsigned int QtVulkanGlImage::glTexture() const { return m_Owner == Owner::Gl ? m_GlTexture : 0; }

bool QtVulkanGlImage::releaseGl(QString& error)
{
    error.clear();
    if (!current() || m_Owner != Owner::Gl) {
        error = QStringLiteral("Qt GPU image is not held for GL sampling"); return false;
    }
    const GLenum layout = GeneralLayout;
    m_Api->signalSemaphore(m_GlReturned, 0, nullptr, 1, &m_GlTexture, &layout);
    auto* gl = m_Context->extraFunctions();
    gl->glFlush(); // submit the signal before Vulkan can wait; not a CPU wait
    if (gl->glGetError() != GL_NO_ERROR) {
        m_Owner = Owner::Failed;
        error = QStringLiteral("Qt GL image ownership return failed"); return false;
    }
    m_Owner = Owner::Returned;
    return true;
}

QtVulkanGlImage::~QtVulkanGlImage()
{
    // Teardown only. There is no un-signalled wait in Vulkan: beginWrite only
    // imports a return semaphore after GL has signalled and flushed it.
    Q_ASSERT(current());
    auto* gl = m_Context->extraFunctions();
    gl->glFinish();
    if (m_Texture && !m_Held) {
        pl_vulkan_hold_params hold{};
        hold.tex = m_Texture; hold.layout = VK_IMAGE_LAYOUT_GENERAL;
        hold.qf = VK_QUEUE_FAMILY_IGNORED; hold.semaphore.sem = m_Ready;
        m_Held = pl_vulkan_hold_ex(m_Vulkan->gpu, &hold);
    }
    pl_gpu_finish(m_Vulkan->gpu);
    pl_tex_destroy(m_Vulkan->gpu, &m_Texture);
    if (m_GlTexture) gl->glDeleteTextures(1, &m_GlTexture);
    if (m_GlMemory) m_Api->deleteMemory(1, &m_GlMemory);
    if (m_GlReady) m_Api->deleteSemaphores(1, &m_GlReady);
    if (m_GlReturned) m_Api->deleteSemaphores(1, &m_GlReturned);
    const auto device = m_Vulkan->device;
    if (m_Ready) vkDestroySemaphore(device, m_Ready, nullptr);
    if (m_Returned) vkDestroySemaphore(device, m_Returned, nullptr);
    if (m_Image) vkDestroyImage(device, m_Image, nullptr);
    if (m_Memory) vkFreeMemory(device, m_Memory, nullptr);
}
