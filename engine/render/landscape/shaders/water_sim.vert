#version 460 core
#include "common.glsl"

// The ONE closed sim-water mesh (docs/WATER-RENDER.md §2): world-space
// vertices built on the worker in extractSnapshot — top faces over wet
// cells plus column-capped side faces at every wet/dry boundary. The
// extent of the water IS this geometry; the fragment side only shades.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv; // sim-texture uv

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vSimUv;

void main() {
    vWorldPos = aPos;
    vSimUv = aUv;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
