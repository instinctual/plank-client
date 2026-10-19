#include "eglvideonode.h"
#include "packedvideoblitter.h"

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

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

namespace {
class VideoNode final : public QSGRenderNode
{
public:
    explicit VideoNode(std::shared_ptr<QtEglVideoStatus> status) : m_Status(std::move(status)) {}
    ~VideoNode() override { releaseResources(); }

    void synchronize(QtVaapiVideoFrame::Ptr frame, const QtSessionGeometry& geometry)
    {
        m_Frame = std::move(frame);
        m_Geometry = geometry;
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
        if (!context) { fail(QtEglVideoStatus::WrongBackend); return; }
        auto* gl = context->extraFunctions();
        // Retire only resources whose final sampling draw has completed. Never
        // wait for a fence during normal rendering and never grow without bound.
        m_Slots.erase(std::remove_if(m_Slots.begin(), m_Slots.end(), [&](auto& slot) {
            if (slot.get() == m_Current) return false;
            const auto state = slot->fence ? gl->glClientWaitSync(slot->fence, 0, 0) : GL_ALREADY_SIGNALED;
            if (state == GL_WAIT_FAILED) { fail(QtEglVideoStatus::FenceFailed); return false; }
            return state == GL_ALREADY_SIGNALED || state == GL_CONDITION_SATISFIED;
        }), m_Slots.end());
        if (!m_Frame) { m_Current = nullptr; return; }
        if (!m_Geometry.valid() || m_Frame->size() != m_Geometry.streamSize ||
                m_Status->error.load() != QtEglVideoStatus::None ||
                (m_Current && m_Current->frame == m_Frame) || m_Slots.size() >= 3)
            return;

        auto slot = std::make_unique<Slot>();
        slot->frame = m_Frame;
        slot->display = eglGetCurrentDisplay();
        auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        slot->destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        using BindImage = void (*)(GLenum, void*);
        auto bindImage = reinterpret_cast<BindImage>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        const auto extensions = QByteArray(eglQueryString(slot->display, EGL_EXTENSIONS));
        const auto tokens = extensions.split(' ');
        if (slot->display == EGL_NO_DISPLAY || !createImage || !slot->destroyImage || !bindImage ||
                !tokens.contains("EGL_EXT_image_dma_buf_import")) {
            fail(QtEglVideoStatus::ImportFailed); return;
        }
        const auto& d = m_Frame->descriptor();
        const auto& p = d.layers[0];
        const auto& object = d.objects[0];
        std::vector<EGLint> attributes = {
            EGL_WIDTH, m_Frame->size().width(), EGL_HEIGHT, m_Frame->size().height(),
            EGL_LINUX_DRM_FOURCC_EXT, EGLint(m_Frame->importFormat()),
            EGL_DMA_BUF_PLANE0_FD_EXT, object.fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(p.offset[0]),
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(p.pitch[0])
        };
        // INVALID means an implicit layout, not a numeric modifier to import.
        if (object.drm_format_modifier != DRM_FORMAT_MOD_INVALID) {
            if (tokens.contains("EGL_EXT_image_dma_buf_import_modifiers")) {
                attributes.insert(attributes.end(), {
                    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGLint(object.drm_format_modifier & 0xffffffff),
                    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGLint(object.drm_format_modifier >> 32)});
            } else if (object.drm_format_modifier != DRM_FORMAT_MOD_LINEAR) {
                fail(QtEglVideoStatus::ImportFailed); return;
            }
        }
        attributes.push_back(EGL_NONE);
        slot->image = createImage(slot->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
        if (slot->image == EGL_NO_IMAGE_KHR) { fail(QtEglVideoStatus::ImportFailed); return; }
        gl->glGenTextures(1, &slot->texture);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, slot->texture);
        bindImage(GL_TEXTURE_EXTERNAL_OES, slot->image);
        gl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        if (!slot->texture || gl->glGetError() != GL_NO_ERROR) { fail(QtEglVideoStatus::ImportFailed); return; }
        m_Current = slot.get();
        m_Slots.push_back(std::move(slot));
        ++m_Status->imports;
    }

    void render(const RenderState*) override
    {
        if (!m_Current || m_Status->error.load() != QtEglVideoStatus::None ||
                !m_Geometry.valid() || m_Current->frame->size() != m_Geometry.streamSize)
            return;
        QString error;
        if (!m_Blitter.draw(m_Current->texture, GL_TEXTURE_EXTERNAL_OES,
                            m_Current->frame->mapping(), m_Geometry, error)) {
            qWarning().noquote() << error;
            fail(QtEglVideoStatus::PrecisionOrDrawFailed);
            return;
        }
        auto* gl = QOpenGLContext::currentContext()->extraFunctions();
        if (m_Current->fence) gl->glDeleteSync(m_Current->fence);
        m_Current->fence = gl->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!m_Current->fence) { fail(QtEglVideoStatus::FenceFailed); return; }
        ++m_Status->draws;
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
        m_Blitter.release();
    }

private:
    struct Slot {
        QtVaapiVideoFrame::Ptr frame;
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
        GLuint texture = 0;
        GLsync fence = nullptr;
        ~Slot()
        {
            if (auto* context = QOpenGLContext::currentContext()) {
                auto* gl = context->extraFunctions();
                if (fence) gl->glDeleteSync(fence);
                if (texture) gl->glDeleteTextures(1, &texture);
            }
            if (image != EGL_NO_IMAGE_KHR && destroyImage) destroyImage(display, image);
        }
    };
    void fail(QtEglVideoStatus::Error error)
    {
        auto expected = QtEglVideoStatus::None;
        if (m_Status->error.compare_exchange_strong(expected, error))
            qWarning("Qt video stopped at the exact-format GPU gate (%d)", int(error));
    }
    QtVaapiVideoFrame::Ptr m_Frame;
    QtSessionGeometry m_Geometry;
    QtPackedVideoBlitter m_Blitter;
    std::shared_ptr<QtEglVideoStatus> m_Status;
    std::vector<std::unique_ptr<Slot>> m_Slots;
    Slot* m_Current = nullptr;
};
}

QtEglVideoItem::QtEglVideoItem(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents);
    setZ(-1);
}

void QtEglVideoItem::setFrame(QtVaapiVideoFrame::Ptr frame)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_Frame = std::move(frame);
    update();
}

void QtEglVideoItem::setPresentation(const QtSessionGeometry& geometry)
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_Geometry = geometry;
    setSize(geometry.logicalSize);
    update();
}

QSGNode* QtEglVideoItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    if (window()->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL) {
        m_Status->error = QtEglVideoStatus::WrongBackend;
        delete oldNode;
        return nullptr;
    }
    auto* node = static_cast<VideoNode*>(oldNode);
    if (!node) node = new VideoNode(m_Status);
    node->synchronize(m_Frame, m_Geometry);
    return node;
}
