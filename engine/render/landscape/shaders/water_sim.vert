#version 460 core
#include "common.glsl"

// Live sim window sheet: a static node-uv grid lifted per vertex by
// the sim's DISPLAY texture (wet surface, or ground minus a tuck where
// dry — the sheet dives under the banks and the depth test cuts the
// exact shoreline; sim ground == render ground, so this is reliable).
layout(location = 0) in vec2 aUv; // grid node in [0,1]²

layout(binding = 7) uniform sampler2D uWaterSimA;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vSimUv;

void main() {
    // Node uv -> texel-centered texture uv (n nodes = n texels).
    vec2 texSize = vec2(textureSize(uWaterSimA, 0));
    vec2 uv = (aUv * (texSize - 1.0) + 0.5) / texSize;
    vSimUv = uv;
    float span = 1.0 / uWaterSimMapInfo.z;
    vec3 world;
    world.x = uWaterSimMapInfo.x + aUv.x * span;
    world.z = uWaterSimMapInfo.y + aUv.y * span;
    world.y = textureLod(uWaterSimA, uv, 0.0).r;
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
