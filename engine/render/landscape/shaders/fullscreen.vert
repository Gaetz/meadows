#version 460 core
#include "compat.glsl"

// Shared fullscreen triangle for post-process passes; identity UV.
layout(location = 0) out vec2 vUv;

void main() {
    vec2 uv = vec2((MEADOWS_VERTEX_INDEX << 1) & 2, MEADOWS_VERTEX_INDEX & 2);
    vUv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
