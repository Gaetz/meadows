#version 460 core
#include "common.glsl"

// Brick 32 — placed water surface quad, built in world space on the CPU.

layout(location = 0) in vec3 aPos;

out vec2 vUv;
out vec3 vWorldPos;

void main() {
    vWorldPos = aPos;
    vUv = aPos.xz * 0.25; // ripple space
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
