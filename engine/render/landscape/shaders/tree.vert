#version 460 core
#include "common.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;      // x = sway weight, y = height01
layout(location = 3) in vec3 aColor;
layout(location = 4) in vec4 aPosScale; // xyz = terrain point, w = scale
layout(location = 5) in vec4 aParams;   // x = yaw, y = tint, z = sway phase,
                                        // w = fade-end distance (m)

out vec3 vNormal;
out vec3 vColor;
out vec3 vWorldPos;
out float vTint;

void main() {
    float yaw = aParams.x;
    float c = cos(yaw);
    float s = sin(yaw);
    vec3 local = vec3(aPos.x * c - aPos.z * s, aPos.y,
                      aPos.x * s + aPos.z * c);
    vec3 normal = vec3(aNormal.x * c - aNormal.z * s, aNormal.y,
                       aNormal.x * s + aNormal.z * c);

    // Distance fade, per category (aParams.w): trees carry to the fog line,
    // rocks and bushes bow out earlier.
    float dist = distance(aPosScale.xyz, uCameraPos.xyz);
    float fade = 1.0 - smoothstep(aParams.w * 0.86, aParams.w, dist);
    vec3 world = aPosScale.xyz + local * (aPosScale.w * fade);

    // Gentle canopy sway: same gust field as the grass, scaled by the
    // per-vertex sway weight (trunk base stays planted).
    float gust = sin(uTime.x * 1.1 + aParams.z +
                     (aPosScale.x + aPosScale.z * 0.7) * 0.05);
    world.xz += vec2(0.9, 0.35) * (gust * 0.07 * aUv.x * aPosScale.w * fade);

    vNormal = normal;
    vColor = aColor;
    vTint = aParams.y;
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
