#version 460 core

// 3x3 box blur for the jittered half-res post targets (speckle fix,
// 2026-07-10): the SSAO and contact-shadow passes rotate their kernels
// per pixel (IGN) precisely to turn banding into FILTERABLE noise —
// this is the filter that was missing. Single channel (R16F targets).
layout(binding = 0) uniform sampler2D uSource;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uSource, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(uSource, vUv + vec2(x, y) * texel).r;
        }
    }
    float value = sum / 9.0;
    fragColor = vec4(value, value, value, 1.0);
}
