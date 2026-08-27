#version 460 core
#include "common.glsl"

// Vertical SKIRTS closing the sim water volume: wherever a water
// column ends against air (flood front, fall lip, dam face), the
// upload emits a quad from the surface straight down to the dry
// neighbour's ground. World-space vertices built on the CPU per
// snapshot; the fragment side is the water_sim shading (the vertical
// facet takes the wall look by its own derivatives).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv; // texel-center uv of the WET cell

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vSimUv;

void main() {
    vWorldPos = aPos;
    vSimUv = aUv;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
