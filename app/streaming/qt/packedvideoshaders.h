#pragma once

// Shared by the production Qt blitter and its offscreen code-value test.
// The caller supplies the desktop/ES version and ES precision declarations.
namespace QtPackedVideoShaders {
inline constexpr const char* vertex = R"glsl(
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
inline constexpr const char* fragment = R"glsl(
uniform sampler2D video;

uniform int mapping;
uniform bool p010;
uniform sampler2D chroma;
in vec2 uv;
out vec4 color;
void main() {
    vec3 sampleValue = texture(video, uv).rgb;
    if (p010) {
        // Plane imports are normalized R16/GR1616, not implicit YUV-to-RGB.
        // P010 stores ten significant bits in the high bits of each word.
        vec2 chromaValue = texture(chroma, uv).rg;
        sampleValue = vec3(chromaValue.g, sampleValue.r, chromaValue.r) * (65535.0 / 65472.0);
    }
    if (mapping == 1) sampleValue = sampleValue.brg; // XR24 sampled Y,U,V -> R=V,G=Y,B=U
    if (mapping == 3 || mapping == 4) {
        float y = sampleValue.g;
        float u = sampleValue.b - 512.0 / 1023.0;
        float v = sampleValue.r - 512.0 / 1023.0;
        if (mapping == 4) {
            y = (sampleValue.g * 1023.0 - 64.0) / 876.0;
            u *= 1023.0 / 896.0;
            v *= 1023.0 / 896.0;
        }
        sampleValue = vec3(y + 1.5748 * v, y - 0.1873242729 * u - 0.4681242729 * v, y + 1.8556 * u);
    }
    color = vec4(sampleValue, 1.0);
}
)glsl";
}
