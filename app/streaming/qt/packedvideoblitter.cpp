#include "packedvideoblitter.h"
#include "packedvideoshaders.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

QtPackedVideoBlitter::~QtPackedVideoBlitter() { release(); }

void QtPackedVideoBlitter::release()
{
    if (auto* context = QOpenGLContext::currentContext())
        if (m_Vao) context->extraFunctions()->glDeleteVertexArrays(1, &m_Vao);
    m_Vao = 0;
    m_Program.reset();
}

bool QtPackedVideoBlitter::initialize(QString& error)
{
    auto* context = QOpenGLContext::currentContext();
    if (!context || context->format().majorVersion() < 3) {
        error = QStringLiteral("Qt video requires a current OpenGL 3+ context and a supported texture");
        return false;
    }
    if (m_Program) return true;
    release();
    const bool es = context->isOpenGLES();
    const QByteArray header = es ? "#version 300 es\n" : "#version 330 core\n";
    const QByteArray vertex = header + QtPackedVideoShaders::vertex;
    QByteArray fragment = header;
    if (es) {
        fragment += "precision highp float;\nprecision highp sampler2D;\n";
    }
    fragment += QtPackedVideoShaders::fragment;
    m_Program = std::make_unique<QOpenGLShaderProgram>();
    if (!m_Program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
            !m_Program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) ||
            !m_Program->link()) {
        error = QStringLiteral("Qt packed video shader failed: %1").arg(m_Program->log());
        release();
        return false;
    }
    context->extraFunctions()->glGenVertexArrays(1, &m_Vao);
    return m_Vao != 0;
}

bool QtPackedVideoBlitter::draw(GLuint texture, PlankPackedVideo::Mapping mapping,
                               const QtSessionGeometry& geometry, QString& error, GLuint chromaTexture,
                               bool concealQualification)
{
    error.clear();
    if (!texture || !geometry.valid() || mapping == PlankPackedVideo::Mapping::Unsupported ||
            (chromaTexture && mapping != PlankPackedVideo::Mapping::Bt709Full10 &&
             mapping != PlankPackedVideo::Mapping::Bt709Limited10)) {
        error = QStringLiteral("Invalid Qt video texture, format or presentation geometry");
        return false;
    }
    if (!initialize(error)) return false;
    auto* context = QOpenGLContext::currentContext();
    auto* gl = context->extraFunctions();
    GLint framebuffer = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    const GLenum attachment = framebuffer ? GL_COLOR_ATTACHMENT0 :
        (context->isOpenGLES() ? GL_BACK : GL_BACK_LEFT);
    const GLenum components[] = {GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE,
                                 GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE,
                                 GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE};
    for (const auto component : components) {
        GLint bits = 0;
        gl->glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, attachment, component, &bits);
        if (bits < (mapping == PlankPackedVideo::Mapping::Identity8 ? 8 : 10)) {
            error = QStringLiteral("Qt video target would reduce the selected color precision");
            return false;
        }
    }
    // Qt restores scene-graph state through begin/endExternalCommands in the
    // adapter. Restore dither/sRGB ourselves: those are outside that contract.
    const bool dither = gl->glIsEnabled(GL_DITHER);
    const bool srgb = !context->isOpenGLES() && gl->glIsEnabled(GL_FRAMEBUFFER_SRGB);
    gl->glDisable(GL_DITHER);
    if (!context->isOpenGLES()) gl->glDisable(GL_FRAMEBUFFER_SRGB);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDepthMask(GL_FALSE);
    gl->glDisable(GL_STENCIL_TEST);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glViewport(0, 0, geometry.drawableSize.width(), geometry.drawableSize.height());
    gl->glBindVertexArray(m_Vao);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindSampler(0, 0);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (chromaTexture) {
        gl->glActiveTexture(GL_TEXTURE1);
        gl->glBindSampler(1, 0);
        gl->glBindTexture(GL_TEXTURE_2D, chromaTexture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    m_Program->bind();
    m_Program->setUniformValue("video", 0);
    m_Program->setUniformValue("mapping", int(mapping));
    m_Program->setUniformValue("p010", chromaTexture != 0);
    m_Program->setUniformValue("chroma", 1);
    const auto& d = geometry.destinationPixels;
    const auto& s = geometry.sourcePixels;
    m_Program->setUniformValue("destination", QVector4D(
        d.x() / geometry.drawableSize.width(), d.y() / geometry.drawableSize.height(),
        d.width() / geometry.drawableSize.width(), d.height() / geometry.drawableSize.height()));
    m_Program->setUniformValue("source", QVector4D(
        s.x() / geometry.streamSize.width(), s.y() / geometry.streamSize.height(),
        s.width() / geometry.streamSize.width(), s.height() / geometry.streamSize.height()));
    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_Program->release();
    gl->glBindVertexArray(0);
    if (chromaTexture) {
        gl->glBindTexture(GL_TEXTURE_2D, 0);
        gl->glActiveTexture(GL_TEXTURE0);
    }
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    if (dither) gl->glEnable(GL_DITHER);
    if (srgb) gl->glEnable(GL_FRAMEBUFFER_SRGB);
    if (gl->glGetError() != GL_NO_ERROR) {
        error = QStringLiteral("Qt packed video draw failed");
        return false;
    }
    if (concealQualification) {
        // Keep the real import/shader/target-precision checks above. Clear in
        // this same GPU command stream before Qt draws controls and swaps;
        // no extra window, framebuffer, delay or CPU readback is needed.
        // glClearBufferfv does not modify Qt's persistent clear-color state.
        const GLfloat black[] = {0, 0, 0, 1};
        gl->glClearBufferfv(GL_COLOR, 0, black);
        if (gl->glGetError() != GL_NO_ERROR) {
            error = QStringLiteral("Qt could not conceal the decoder qualification image");
            return false;
        }
    }
    return true;
}
