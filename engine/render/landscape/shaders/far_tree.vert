#version 460 core
#include "compat.glsl"
#include "common.glsl"

// Far-tree impostors (FarTerrain): one cylindrical billboard per
// instance, corners from the vertex index (no per-vertex buffer). Fades
// in where the real trees end and out into the veil — the dissolve
// happens in the fragment (depth-write-friendly dither discard).
// Two looks share the quad (uImpostorInfo.x): the baked atlas
// (TreeImpostorAtlas — the real variant rendered from 16 directions;
// the quad spans the tile's box, 2 x kBoxHalf of the tree's "fit"
// unit) and the analytic discs (the A/B).
layout(location = 0) in vec4 aPosScale; // xyz ground point, w height (m)
layout(location = 1) in vec4 aParams;   // x crown seed, y variant + tint jitter,
                                        // z width ratio (< 0 = conifer),
                                        // w trunk fraction

// Mirrors FarTerrain::ImpostorPush (change both together).
MEADOWS_PUSH_CONSTANTS(ImpostorPush) {
    vec4 uImpostorInfo;       // x atlas on, y azimuths, z rings, w ring step (rad)
    vec4 uImpostorVariant[5]; // x fit/height, y crown start/height
};

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vUv; // x in [-0.5,0.5], y in [0,1]
layout(location = 2) out vec4 vParams;
layout(location = 3) out float vFade;
layout(location = 4) out vec3 vToCam;

const float kBoxHalf = 0.75;

void main() {
    const vec2 kCorners[6] =
        vec2[6](vec2(-0.5, 0.0), vec2(0.5, 0.0), vec2(0.5, 1.0),
                vec2(-0.5, 0.0), vec2(0.5, 1.0), vec2(-0.5, 1.0));
    vec2 c = kCorners[MEADOWS_VERTEX_INDEX % 6];
    vec3 ground = aPosScale.xyz;
    float h = aPosScale.w;
    vec3 toCam = uCameraPos.xyz - ground;
    float d = length(toCam.xz);
    vec3 right = normalize(vec3(toCam.z, 0.0, -toCam.x));
    vec3 p;
    if (uImpostorInfo.x > 0.5) {
        // The tile box in metres: one fit unit = the tree's height
        // times the variant's fit/height; the box is +-kBoxHalf
        // around half a unit up.
        int variant = int(clamp(floor(aParams.y), 0.0, 4.0));
        float unit = h * max(uImpostorVariant[variant].x, 1.0);
        float side = 2.0 * kBoxHalf * unit;
        p = ground + right * (c.x * side) +
            vec3(0.0, (0.5 - kBoxHalf) * unit + c.y * side, 0.0);
    } else {
        // Width comes from the measured crown ratio of the real trees.
        p = ground + right * (c.x * h * abs(aParams.z)) +
            vec3(0.0, c.y * h, 0.0);
    }
    vWorldPos = p;
    vUv = c;
    vParams = aParams;
    vToCam = toCam;
    // In past the real-tree fade (uVegInfo.z tracks the vegetation
    // ring), out into the fog veil. The fade-out ends at kFarTreeFar, the
    // INNER ring's Config.treeFar (FarTerrain, WorldRenderer's create):
    // the bake stops scattering impostors there — change both together.
    float fadeEnd = max(uVegInfo.z, 100.0);
    vFade = smoothstep(fadeEnd * 0.8, fadeEnd + 20.0, d) *
            (1.0 - smoothstep(kFarTreeFar - 1000.0, kFarTreeFar, d));
    gl_Position = uViewProj * vec4(p, 1.0);
}
