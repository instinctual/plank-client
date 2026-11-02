#include "eglvideonode.h"
#include "packedvideoblitter.h"
#include "egltextureimport.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QQuickWindow>
#include <QSGRenderNode>
#include <QThread>
#include <algorithm>
#include <vector>

namespace {
class VideoNode final : public QSGRenderNode
{
public:
    explicit VideoNode(std::shared_ptr<QtVideoStatus> status) : m_Status(std::move(status)) {}
    ~VideoNode() override { releaseResources(); }
    bool hasStatus(const std::shared_ptr<QtVideoStatus>& status) const { return m_Status == status; }

    void synchronize(QtVideoFrame::Ptr frame, const QtSessionGeometry& geometry,
                     QtSoftwareVideoDevice::Ptr device)
    {
        m_Frame = std::move(frame);
        m_Geometry = geometry;
        m_SoftwareDevice = std::move(device);
        markDirty(DirtyMaterial);
    }

    QRectF rect() const override { return QRectF(QPointF(), m_Geometry.logicalSize); }
    RenderingFlags flags() const override { return BoundedRectRendering; }
    StateFlags changedStates() const override
    {
        return BlendState | DepthState | StencilState | ScissorState | CullState | ColorState | ViewportState;
    }

    void prepare() override
    {
        auto* context = QOpenGLContext::currentContext();
        if (!context) { fail(QtVideoStatus::WrongBackend); return; }
        auto* gl = context->extraFunctions();
        if (m_Frame && m_Frame->softwareFrame()) {
            prepareSoftware();
            return;
        }
        // Retire only resources whose final sampling draw has completed. Never
        // wait for a fence during normal rendering and never grow without bound.
        m_Slots.erase(std::remove_if(m_Slots.begin(), m_Slots.end(), [&](auto& slot) {
            if (slot.get() == m_Current) return false;
            const auto state = slot->fence ? gl->glClientWaitSync(slot->fence, 0, 0) : GL_ALREADY_SIGNALED;
            if (state == GL_WAIT_FAILED) { fail(QtVideoStatus::FenceFailed); return false; }
            return state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED;
        }), m_Slots.end());
        if (!m_Frame) { m_Current = nullptr; return; }
        if (!m_Geometry.valid() || m_Frame->size() != m_Geometry.streamSize ||
                m_Status->error.load() != QtVideoStatus::None ||
                (m_Current && m_Current->frame == m_Frame->hardwareFrame()) || m_Slots.size() >= 3)
            return;

        auto slot = std::make_unique<Slot>();
        slot->frame = m_Frame->hardwareFrame();
        slot->delivery = m_Frame;
        if (!slot->frame) { fail(QtVideoStatus::ImportFailed); return; }
        QString importError;
        if (!m_Importer.prepare(importError) || !m_Importer.hasDmaBufImport()) {
            qWarning().noquote() << "Qt VA-API import requires EGL DMA-BUF image support" << importError;
            fail(QtVideoStatus::ImportFailed); return;
        }
        slot->display = m_Importer.display();
        slot->destroyImage = m_Importer.destroyImage();
        const auto& d = slot->frame->descriptor();
        const auto& p = d.layers[0];
        for (unsigned index = 0; index < p.num_planes; ++index) {
            const auto& object = d.objects[p.object_index[index]];
            const auto size = slot->frame->planeSize(index);
            std::vector<EGLint> attributes = {
                EGL_WIDTH, size.width(), EGL_HEIGHT, size.height(),
                EGL_LINUX_DRM_FOURCC_EXT, EGLint(slot->frame->importFormat(index)),
                EGL_DMA_BUF_PLANE0_FD_EXT, object.fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(p.offset[index]),
                EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(p.pitch[index])
            };
            // INVALID means an implicit layout, not a numeric modifier to import.
            if (object.drm_format_modifier != DRM_FORMAT_MOD_INVALID) {
                if (m_Importer.hasModifiers()) {
                    attributes.insert(attributes.end(), {
                        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGLint(object.drm_format_modifier & 0xffffffff),
                        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGLint(object.drm_format_modifier >> 32)});
                } else if (object.drm_format_modifier != DRM_FORMAT_MOD_LINEAR) {
                    qWarning("Qt VA-API import requires EGL modifier support for format 0x%x",
                             slot->frame->importFormat(index));
                    fail(QtVideoStatus::ImportFailed); return;
                }
            }
            attributes.push_back(EGL_NONE);
            slot->images[index] = m_Importer.createImage()(slot->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
            if (slot->images[index] == EGL_NO_IMAGE_KHR) {
                qWarning("Qt VA-API plane %u image creation failed (format 0x%x, modifier 0x%llx, EGL 0x%x)",
                         index, slot->frame->importFormat(index),
                         static_cast<unsigned long long>(object.drm_format_modifier), eglGetError());
                fail(QtVideoStatus::ImportFailed); return;
            }
            QString error;
            if (!m_Importer.importTexture(slot->images[index], slot->textures[index], error)) {
                qWarning().noquote() << error << "VA-API plane" << index;
                fail(QtVideoStatus::ImportFailed); return;
            }
        }
        m_Current = slot.get();
        m_Slots.push_back(std::move(slot));
        ++m_Status->imports;
    }

    void render(const RenderState*) override
    {
        const auto started = std::chrono::steady_clock::now();
        if (m_Frame && m_Frame->softwareFrame()) {
            if (!m_SoftwareImage || m_Status->error.load() != QtVideoStatus::None ||
                    !m_Geometry.valid() || m_Frame->size() != m_Geometry.streamSize) return;
            QString error;
            if (!m_Blitter.draw(m_SoftwareImage->glTexture(),
                                PlankPackedVideo::Mapping::Identity10, m_Geometry, error, 0,
                                m_ConvertedFrame->isQualification())) {
                qWarning().noquote() << error;
                fail(QtVideoStatus::PrecisionOrDrawFailed);
                return;
            }
            ++m_Status->draws;
            recordDraw(m_ConvertedFrame, started);
            return;
        }
        if (!m_Current || m_Status->error.load() != QtVideoStatus::None ||
                !m_Geometry.valid() || m_Current->frame->size() != m_Geometry.streamSize)
            return;
        QString error;
        if (!m_Blitter.draw(m_Current->textures[0],
                            m_Current->frame->mapping(), m_Geometry, error, m_Current->textures[1],
                            m_Current->delivery->isQualification())) {
            qWarning().noquote() << error;
            fail(QtVideoStatus::PrecisionOrDrawFailed);
            return;
        }
        auto* gl = QOpenGLContext::currentContext()->extraFunctions();
        if (m_Current->fence) gl->glDeleteSync(m_Current->fence);
        m_Current->fence = gl->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!m_Current->fence) { fail(QtVideoStatus::FenceFailed); return; }
        ++m_Status->draws;
        recordDraw(m_Current->delivery, started);
    }

    void releaseResources() override
    {
        // Teardown only: finish outstanding sampling before releasing decoded
        // surfaces to their pool. No per-frame glFinish/readback is used.
        if (!m_Slots.empty()) {
            if (auto* context = QOpenGLContext::currentContext()) context->extraFunctions()->glFinish();
        }
        m_Slots.clear();
        m_Current = nullptr;
        m_SoftwareImage.reset(); // final sampling drains only during teardown
        m_ConvertedFrame.reset();
        m_ImageDevice.reset();
        m_Blitter.release();
        m_Importer.reset();
    }

private:
    void recordDraw(QtVideoFrame::Ptr frame, std::chrono::steady_clock::time_point started)
    {
        QtVideoFrame::Ptr retired;
        {
            std::lock_guard<std::mutex> lock(m_Status->presentationMutex);
            retired = std::move(m_Status->drawnFrame);
            m_Status->drawnFrame = std::move(frame);
            m_Status->drawStarted = started;
        }
    }
    void prepareSoftware()
    {
        if (m_Status->error.load() != QtVideoStatus::None || !m_Geometry.valid() ||
                m_Frame->size() != m_Geometry.streamSize || m_Frame == m_ConvertedFrame) return;
        if (!m_SoftwareDevice) { fail(QtVideoStatus::ImportFailed); return; }
        if (m_ImageDevice != m_SoftwareDevice ||
                (m_SoftwareImage && m_SoftwareImage->size() != m_Frame->size())) {
            m_SoftwareImage.reset();
            m_ConvertedFrame.reset();
            m_ImageDevice = m_SoftwareDevice;
        }
        QString error;
        if (!m_SoftwareImage) {
            m_SoftwareImage = m_SoftwareDevice->createImage(m_Frame->size(), error);
            if (!m_SoftwareImage) {
                qWarning().noquote() << error;
                fail(QtVideoStatus::ImportFailed); return;
            }
            ++m_Status->imports;
        } else if (!m_SoftwareImage->releaseGl(error)) {
            qWarning().noquote() << error;
            fail(QtVideoStatus::FenceFailed); return;
        }
        if (!m_SoftwareDevice->convert(m_Frame->softwareFrame(), m_Frame->softwareFormat(),
                                      *m_SoftwareImage, error)) {
            qWarning().noquote() << error;
            fail(QtVideoStatus::PrecisionOrDrawFailed); return;
        }
        m_ConvertedFrame = m_Frame;
    }
    struct Slot {
        QtVaapiVideoFrame::Ptr frame;
        QtVideoFrame::Ptr delivery;
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLImageKHR images[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
        PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
        GLuint textures[2] = {};
        GLsync fence = nullptr;
        ~Slot()
        {
            if (auto* context = QOpenGLContext::currentContext()) {
                auto* gl = context->extraFunctions();
                if (fence) gl->glDeleteSync(fence);
                gl->glDeleteTextures(2, textures);
            }
            for (auto image : images)
                if (image != EGL_NO_IMAGE_KHR && destroyImage) destroyImage(display, image);
        }
    };
    void fail(QtVideoStatus::Error error)
    {
        auto expected = QtVideoStatus::None;
        if (m_Status->error.compare_exchange_strong(expected, error))
            qWarning("Qt video stopped at the exact-format GPU gate (%d)", int(error));
        std::lock_guard<std::mutex> lock(m_Status->presentationMutex);
        m_Status->presentationChanged.notify_all();
    }
    QtVideoFrame::Ptr m_Frame;
    QtVideoFrame::Ptr m_ConvertedFrame;
    QtSoftwareVideoDevice::Ptr m_SoftwareDevice, m_ImageDevice;
    std::unique_ptr<QtVulkanGlImage> m_SoftwareImage;
    QtSessionGeometry m_Geometry;
    QtPackedVideoBlitter m_Blitter;
    QtEglTextureImporter m_Importer;
    std::shared_ptr<QtVideoStatus> m_Status;
    std::vector<std::unique_ptr<Slot>> m_Slots;
    Slot* m_Current = nullptr;
};
}

void QtEglVideoItem::setSoftwareDevice(QtSoftwareVideoDevice::Ptr device)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_SoftwareDevice = std::move(device);
    update();
}

QSGNode* QtEglVideoItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    ++m_Status->synchronizations;
    if (window()->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
        m_Status->error = QtVideoStatus::WrongBackend;
        delete oldNode;
        return nullptr;
    }
    auto* node = static_cast<VideoNode*>(oldNode);
    if (node && !node->hasStatus(m_Status)) { delete node; node = nullptr; }
    if (!node) node = new VideoNode(m_Status);
    node->synchronize(m_Frame, m_Geometry, m_SoftwareDevice);
    return node;
}
