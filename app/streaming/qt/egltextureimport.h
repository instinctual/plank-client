#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QOpenGLContext>
#include <QPointer>
#include <QString>

// Render-thread only. Cache driver entry points and extension capabilities for
// one Qt/EGL context. No decoded frame, image, texture or fence is cached here.
class QtEglTextureImporter
{
public:
    bool prepare(QString& error);
    void reset();
    bool hasDmaBufImport() const { return m_HasDmaBufImport; }
    bool hasModifiers() const { return m_HasModifiers; }
    EGLDisplay display() const { return m_Display; }
    PFNEGLCREATEIMAGEKHRPROC createImage() const { return m_CreateImage; }
    PFNEGLDESTROYIMAGEKHRPROC destroyImage() const { return m_DestroyImage; }

    // Import raw RGB/R/RG storage without implicit YUV conversion or a CPU
    // copy. Caller owns the returned GL_TEXTURE_2D. Failure leaves texture zero
    // and preserves the current binding. prepare() must succeed first.
    bool importTexture(EGLImageKHR image, GLuint& texture, QString& error) const;

private:
    using BindImage = void (*)(GLenum, void*);
    QPointer<QOpenGLContext> m_Context;
    EGLContext m_EglContext = EGL_NO_CONTEXT;
    EGLDisplay m_Display = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC m_CreateImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC m_DestroyImage = nullptr;
    BindImage m_BindImage = nullptr;
    bool m_HasDmaBufImport = false;
    bool m_HasModifiers = false;
};
