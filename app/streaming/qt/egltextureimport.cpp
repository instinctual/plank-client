#include "egltextureimport.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

void QtEglTextureImporter::reset()
{
    m_Context.clear();
    m_EglContext = EGL_NO_CONTEXT;
    m_Display = EGL_NO_DISPLAY;
    m_CreateImage = nullptr;
    m_DestroyImage = nullptr;
    m_BindImage = nullptr;
    m_HasDmaBufImport = false;
    m_HasModifiers = false;
}

bool QtEglTextureImporter::prepare(QString& error)
{
    error.clear();
    auto* context = QOpenGLContext::currentContext();
    const auto display = eglGetCurrentDisplay();
    const auto eglContext = eglGetCurrentContext();
    if (context && m_Context == context && m_Display == display &&
            m_EglContext == eglContext && m_BindImage) return true;
    reset();
    if (!context || display == EGL_NO_DISPLAY || eglContext == EGL_NO_CONTEXT) {
        error = QStringLiteral("Qt EGL import requires a current Qt/EGL context");
        return false;
    }
    const auto create = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    const auto destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    const auto bind = reinterpret_cast<BindImage>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!create || !destroy || !bind || !extensions) {
        error = QStringLiteral("Qt EGL image import entry points are unavailable");
        return false;
    }
    const auto tokens = QByteArray(extensions).split(' ');
    m_Context = context;
    m_EglContext = eglContext;
    m_Display = display;
    m_CreateImage = create;
    m_DestroyImage = destroy;
    m_BindImage = bind;
    m_HasDmaBufImport = tokens.contains("EGL_EXT_image_dma_buf_import");
    m_HasModifiers = tokens.contains("EGL_EXT_image_dma_buf_import_modifiers");
    return true;
}

bool QtEglTextureImporter::importTexture(EGLImageKHR image, GLuint& texture, QString& error) const
{
    error.clear();
    auto* context = QOpenGLContext::currentContext();
    if (!context || m_Context != context || image == EGL_NO_IMAGE_KHR || texture || !m_BindImage) {
        error = QStringLiteral("Qt EGL import requires a current context, image and empty texture slot");
        return false;
    }
    auto* gl = context->extraFunctions();
    const auto previousError = gl->glGetError();
    if (previousError != GL_NO_ERROR) {
        error = QStringLiteral("Qt EGL import found an earlier GL error 0x%1").arg(previousError, 0, 16);
        return false;
    }
    GLint previousTexture = 0;
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    GLuint imported = 0;
    gl->glGenTextures(1, &imported);
    gl->glBindTexture(GL_TEXTURE_2D, imported);
    // All accepted VA-API planes are reinterpreted as RGB, R16 or GR1616.
    // They are ordinary 2D textures. EXTERNAL_OES is not a desktop OpenGL
    // texture target on the qualified Intel driver (GL_INVALID_ENUM).
    m_BindImage(GL_TEXTURE_2D, image);
    const auto importError = gl->glGetError();
    gl->glBindTexture(GL_TEXTURE_2D, GLuint(previousTexture));
    if (!imported || importError != GL_NO_ERROR) {
        gl->glDeleteTextures(1, &imported);
        error = QStringLiteral("Qt EGL image-to-2D-texture import failed (GL 0x%1)").arg(importError, 0, 16);
        return false;
    }
    texture = imported;
    return true;
}
