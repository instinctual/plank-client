#include "egltextureimport.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

bool importQtEglTexture(EGLImageKHR image, GLuint& texture, QString& error)
{
    error.clear();
    auto* context = QOpenGLContext::currentContext();
    using BindImage = void (*)(GLenum, void*);
    auto bindImage = reinterpret_cast<BindImage>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!context || image == EGL_NO_IMAGE_KHR || texture || !bindImage) {
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
    bindImage(GL_TEXTURE_2D, image);
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
