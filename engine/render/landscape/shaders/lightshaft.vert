#version 460 core
#include "common.glsl"

// Brick 34 — dust light shaft: the prism is built in WORLD SPACE on the
// CPU (a few crossed blades from the light's position/direction/angle),
// so the vertex stage is a pure passthrough. uv.x = -1..1 across the
// blade (radial fade), uv.y = 0..1 along the axis (axial fade + scroll).

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vWorldPos;

void main() {
    vUv = aUv;
    vWorldPos = aPos;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
