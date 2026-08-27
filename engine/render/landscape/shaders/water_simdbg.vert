#version 460 core
#include "common.glsl"

// Debug water-volume boxes: one translucent column per wet sim cell,
// from the ground to the simulated surface — "where the water IS",
// independent of every shading subtlety. uWaterSimTuneInfo.w = texel.
layout(location = 0) in vec3 aPos;  // unit cube [0,1]^3
layout(location = 1) in vec4 aCell; // x, yBase, z, height (per instance)

layout(location = 0) out float vHeight;
layout(location = 1) out vec3 vLocal;

void main() {
    float texel = max(uWaterSimTuneInfo.w, 0.5);
    vec3 world;
    world.x = aCell.x + (aPos.x - 0.5) * texel * 0.92;
    world.z = aCell.z + (aPos.z - 0.5) * texel * 0.92;
    world.y = aCell.y + aPos.y * aCell.w;
    vHeight = aCell.w;
    vLocal = aPos;
    gl_Position = uViewProj * vec4(world, 1.0);
}
