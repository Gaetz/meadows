#version 460 core
#include "common.glsl"

layout(binding = 0) uniform sampler2D uSceneColor;

in vec2 vUv;
out vec4 fragColor;

// ACES-fitted filmic curve (Krzysztof Narkowicz).
vec3 acesFilm(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(uSceneColor, vUv).rgb * uPostInfo.y;
    // A/B toggle: raw path clips instead of rolling off (same gamma encode,
    // so the comparison isolates the tonemap curve).
    vec3 color = uPostInfo.x > 0.5 ? acesFilm(hdr) : clamp(hdr, 0.0, 1.0);
    // Manual gamma encode — no global GL_FRAMEBUFFER_SRGB, the 2D sprite
    // path stays untouched.
    color = pow(color, vec3(1.0 / 2.2));
    fragColor = vec4(color, 1.0);
}
