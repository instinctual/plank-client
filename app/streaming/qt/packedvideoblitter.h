#pragma once

#include "sessiongeometry.h"
#include "../video/packedvideo.h"

#include <QOpenGLShaderProgram>
#include <memory>

// Render-thread-only drawing into Qt's already-bound target. No SDL context,
// swapchain, native window or CPU image is created here. Used by the Qt EGL
// importer and by the exact-code-value GPU fixture.
class QtPackedVideoBlitter final
{
public:
    ~QtPackedVideoBlitter();
    bool draw(GLuint texture, PlankPackedVideo::Mapping mapping,
              const QtSessionGeometry& geometry, QString& error, GLuint chromaTexture = 0);
    void release();

private:
    bool initialize(QString& error);
    std::unique_ptr<QOpenGLShaderProgram> m_Program;
    GLuint m_Vao = 0;
};
