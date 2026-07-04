#version 460 core
#include "common.glsl"

// Leaf cards over the tree canopy (halisavakis stylized-leaves recipe):
// STATIC quads baked in mesh space (the article's Habrador/MTree-style
// placement — cheaper on fill-rate than billboards, which always present
// their full area). Same instance stream as tree.vert. aNormal is the
// SPHERICAL normal (blob-center direction), shared by the four corners —
// it lights the canopy as one soft volume.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;       // leaf-bouquet atlas coords
layout(location = 3) in vec3 aColor;
layout(location = 4) in vec4 aPosScale; // xyz = terrain point, w = scale
layout(location = 5) in vec4 aParams;   // x = yaw, y = tint, z = sway phase,
                                        // w = fade-end distance (m)

out vec3 vNormal;
out vec3 vColor;
out vec3 vWorldPos;
out vec2 vUv;
out float vTint;

void main() {
    float yaw = aParams.x;
    float c = cos(yaw);
    float s = sin(yaw);
    vec3 local = vec3(aPos.x * c - aPos.z * s, aPos.y,
                      aPos.x * s + aPos.z * c);
    vec3 normal = vec3(aNormal.x * c - aNormal.z * s, aNormal.y,
                       aNormal.x * s + aNormal.z * c);

    float dist = distance(aPosScale.xyz, uCameraPos.xyz);
    float fade = 1.0 - smoothstep(aParams.w * 0.86, aParams.w, dist);
    vec3 world = aPosScale.xyz + local * (aPosScale.w * fade);

    // Canopy gust (same field as tree.vert, full weight up here) + a small
    // per-card flutter, phase hashed from the card's shared normal (free
    // per-card random).
    float gust = sin(uWindInfo.x * 1.1 + aParams.z +
                     (aPosScale.x + aPosScale.z * 0.7) * 0.05);
    float cardPhase = dot(aNormal, vec3(12.9898, 78.233, 37.719));
    float flutter = sin(uWindInfo.x * 3.7 + cardPhase);
    world.xz += vec2(0.9, 0.35) *
                (gust * 0.07 * uWindInfo.y * aPosScale.w * fade);
    world += normal * (flutter * 0.035 * uWindInfo.y * aPosScale.w * fade);

    vNormal = normal;
    vColor = aColor;
    vUv = aUv;
    vTint = aParams.y;
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
