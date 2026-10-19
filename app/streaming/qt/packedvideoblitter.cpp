#include "packedvideoblitter.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

QtPackedVideoBlitter::~QtPackedVideoBlitter() { release(); }

void QtPackedVideoBlitter::release()
{
    if (auto* context = QOpenGLContext::currentContext())
        if (m_Vao) context->extraFunctions()->glDeleteVertexArrays(1, &m_Vao);
    m_Vao = 0;
    m_Program.reset();
    m_TextureTarget = 0;
}

bool QtPackedVideoBlitter::initialize(GLenum target, QString& error)
{
    auto* context = QOpenGLContext::currentContext();
    if (!context || context->format().majorVersion() < 3 ||
            (target != GL_TEXTURE_2D && target != GL_TEXTURE_EXTERNAL_OES)) {
        error = QStringLiteral("Qt video requires a current OpenGL 3+ context and a supported texture");
        return false;
    }
    if (m_Program && m_TextureTarget == target) return true;
    release();
    const bool es = context->isOpenGLES();
    const bool external = target == GL_TEXTURE_EXTERNAL_OES;
    const QByteArray header = es ? "#version 300 es\n" : "#version 330 core\n";
    const QByteArray vertex = header + R"glsl(
uniform vec4 destination;
uniform vec4 source;
out vec2 uv;
void main() {
    vec2 corner = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : 0.0,
                      gl_VertexID >= 2 ? 1.0 : 0.0);
    vec2 position = destination.xy + corner * destination.zw;
    gl_Position = vec4(position.x * 2.0 - 1.0, 1.0 - position.y * 2.0, 0.0, 1.0);
    uv = source.xy + corner * source.zw;
}
)glsl";
    QByteArray fragment = header;
    if (external) fragment += es ? "#extension GL_OES_EGL_image_external_essl3 : require\n" :
                                  "#extension GL_OES_EGL_image_external : require\n";
    if (es) fragment += "precision highp float;\n";
    fragment += external ? "uniform samplerExternalOES video;\n" : "uniform sampler2D video;\n";
    fragment += R"glsl(
uniform int mapping;
in vec2 uv;
out vec4 color;
void main() {
    vec3 sampleValue = texture(video, uv).rgb;
    if (mapping == 1) sampleValue = sampleValue.brg; // XR24 sampled Y,U,V -> R=V,G=Y,B=U
    if (mapping == 3) {
        float y = sampleValue.g;
        float u = sampleValue.b - 512.0 / 1023.0;
        float v = sampleValue.r - 512.0 / 1023.0;
        sampleValue = vec3(y + 1.5748 * v, y - 0.1873242729 * u - 0.4681242729 * v, y + 1.8556 * u);
    }
    color = vec4(sampleValue, 1.0);
}
)glsl";
    m_Program = std::make_unique<QOpenGLShaderProgram>();
    if (!m_Program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) ||
            !m_Program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) ||
            !m_Program->link()) {
        error = QStringLiteral("Qt packed video shader failed: %1").arg(m_Program->log());
        release();
        return false;
    }
    context->extraFunctions()->glGenVertexArrays(1, &m_Vao);
    m_TextureTarget = target;
    return m_Vao != 0;
}

bool QtPackedVideoBlitter::draw(GLuint texture, GLenum target, PlankPackedVideo::Mapping mapping,
                               const QtSessionGeometry& geometry, QString& error)
{
    error.clear();
    if (!texture || !geometry.valid() || mapping == PlankPackedVideo::Mapping::Unsupported) {
        error = QStringLiteral("Invalid Qt video texture, format or presentation geometry");
        return false;
    }
    if (!initialize(target, error)) return false;
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
    gl->glDisable(GL_STENCIL_TEST);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl->glViewport(0, 0, geometry.drawableSize.width(), geometry.drawableSize.height());
    gl->glBindVertexArray(m_Vao);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindSampler(0, 0);
    gl->glBindTexture(target, texture);
    gl->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_Program->bind();
    m_Program->setUniformValue("video", 0);
    m_Program->setUniformValue("mapping", int(mapping));
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
    gl->glBindTexture(target, 0);
    if (dither) gl->glEnable(GL_DITHER);
    if (srgb) gl->glEnable(GL_FRAMEBUFFER_SRGB);
    if (gl->glGetError() != GL_NO_ERROR) {
        error = QStringLiteral("Qt packed video draw failed");
        return false;
    }
    return true;
}
