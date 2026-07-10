#version 460 core
#include "common.glsl"

// Brick 30 — horizon cumulonimbus towers: giant camera-anchored
// billboards on the horizon ring (they never get closer — the ring
// follows uCameraPos, yaw-only facing). Static vertex buffer; attributes
// pack {azimuth, u (-1..1), v (0..1), seed}.

layout(location = 0) in vec4 aTower;

out vec2 vUv;
out float vSeed;
out vec3 vWorldPos;

void main() {
    float azimuth = aTower.x;
    float u = aTower.y;
    float v = aTower.z;
    vSeed = aTower.w;

    // Ring INSIDE the camera far plane (dev report 2026-07-10: at the
    // original 3200 m the towers sat beyond farPlane=1600 and were
    // clipped SINCE DAY ONE — brick 7.6 had never actually displayed).
    // Sizes scale with the ring (same perspective, same on-screen
    // footprint); still behind every drawn chunk (~960 m), so terrain
    // silhouettes keep occluding them.
    float ring = 1400.0; // meters out
    float width = (310.0 + vSeed * 390.0);
    float height = (400.0 + vSeed * 480.0);
    vec3 dir = vec3(cos(azimuth), 0.0, sin(azimuth));
    vec3 right = vec3(-dir.z, 0.0, dir.x);
    vec3 base = uCameraPos.xyz + dir * ring;
    base.y = -80.0; // roots below any terrain silhouette
    vec3 world = base + right * (u * width) + vec3(0.0, v * height, 0.0);

    vUv = vec2(u, v);
    vWorldPos = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
