#version 460 core
#include "compat.glsl"
#include "common.glsl"

// Flame tongues (docs/FIRE-RENDER.md F2): an UPRIGHT quad anchored at
// the particle's position, facing the camera around the world up axis
// (a cylindrical billboard — flames never lie down), at the flipbook
// frame's aspect. Six verts per instance from MEADOWS_VERTEX_INDEX,
// instance data from the FxInstances SSBO the FxRenderer packs per batch.

layout(std430, binding = 2) readonly buffer FxInstances {
    vec4 data[]; // quads: [posSize, core+age, outer+seed, life] per flame
};

MEADOWS_PUSH_CONSTANTS(FxPush) {
    ivec4 uFxBase;
};

layout(location = 0) out vec2 vUv;     // x -1..1 across, y 0..1 base->tip
layout(location = 1) out vec4 vCore;   // rgb core colour, a = age 0..1
layout(location = 2) out vec4 vOuter;  // rgb outer colour, a = seed
layout(location = 3) out vec4 vLife;   // x = lifetime (s)
layout(location = 4) out vec3 vWorldPos;

void main() {
    int particle = MEADOWS_VERTEX_INDEX / 6 + uFxBase.x;
    int corner = MEADOWS_VERTEX_INDEX % 6;
    vec4 posSize = data[particle * 4 + 0];
    vCore = data[particle * 4 + 1];
    vOuter = data[particle * 4 + 2];
    vLife = data[particle * 4 + 3];

    vec3 toCam = uCameraPos.xyz - posSize.xyz;
    toCam.y = 0.0;
    float len = length(toCam);
    toCam = len > 1e-4 ? toCam / len : vec3(0.0, 0.0, 1.0);
    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), toCam));

    vec2 uv = (corner == 0) ? vec2(-1.0, 0.0)
              : (corner == 1) ? vec2(1.0, 0.0)
              : (corner == 2) ? vec2(1.0, 1.0)
              : (corner == 3) ? vec2(-1.0, 0.0)
              : (corner == 4) ? vec2(1.0, 1.0)
                              : vec2(-1.0, 1.0);
    vUv = uv;
    float height = posSize.w;
    // The frame keeps the sheet's aspect.
    float halfWidth = height * max(uFireFlameInfo.w, 0.05) * 0.5;
    vec3 world = posSize.xyz + right * (uv.x * halfWidth) +
                 vec3(0.0, uv.y * height, 0.0);
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
