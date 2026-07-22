#version 460 core
#include "common.glsl"

// Placed water surface quad, built in world space on the CPU.

layout(location = 0) in vec3 aPos;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vWorldPos;

void main() {
    vWorldPos = aPos;
    vUv = aPos.xz * 0.25; // ripple space
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
