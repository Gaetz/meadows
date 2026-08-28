#version 460 core
#include "common.glsl"

// Frozen sim window: the SAME closed mesh as water_sim.vert, drawn
// from a persistent snapshot of a past window state (world-space
// vertices, nothing to displace).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv; // the FROZEN window's texel uv

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vSimUv;

void main() {
    vWorldPos = aPos;
    vSimUv = aUv;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
