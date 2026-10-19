#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <QtGui/qopengl.h>
#include <QString>

// Render-thread only. Import raw RGB/R/RG storage, without implicit YUV
// conversion or a CPU copy. The caller owns the returned GL_TEXTURE_2D.
// On failure texture remains zero; the current texture binding is preserved.
bool importQtEglTexture(EGLImageKHR image, GLuint& texture, QString& error);
