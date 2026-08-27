#version 460 core
#include "common.glsl"

layout(location = 0) in float vHeight;
layout(location = 1) in vec3 vLocal;
layout(location = 0) out vec4 fragColor;

void main() {
    // Column color by height (shallow cyan -> deep blue), edges darkened
    // so the boxes read as boxes.
    vec3 color = mix(vec3(0.20, 0.90, 1.00), vec3(0.05, 0.25, 0.60),
                     clamp(vHeight * 0.25, 0.0, 1.0));
    vec2 e = min(vLocal.xz, 1.0 - vLocal.xz);
    float edge = smoothstep(0.0, 0.08, min(e.x, e.y));
    color *= mix(0.45, 1.0, edge);
    fragColor = vec4(color, 0.42);
}
