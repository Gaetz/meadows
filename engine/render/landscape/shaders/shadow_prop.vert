#version 460 core
#include "common.glsl"

// Mirrors tree.vert's placement (yaw, scale, distance fade, canopy sway) so
// shadows match the drawn geometry; lighting-only outputs dropped.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUv;      // x = sway weight
layout(location = 4) in vec4 aPosScale;
layout(location = 5) in vec4 aParams;

layout(std140, binding = 1) uniform ShadowUbo { mat4 uLightViewProj; };

void main() {
    float yaw = aParams.x;
    float c = cos(yaw);
    float s = sin(yaw);
    vec3 local = vec3(aPos.x * c - aPos.z * s, aPos.y,
                      aPos.x * s + aPos.z * c);

    float dist = distance(aPosScale.xyz, uCameraPos.xyz);
    float fade = 1.0 - smoothstep(380.0, 440.0, dist);
    vec3 world = aPosScale.xyz + local * (aPosScale.w * fade);

    float gust = sin(uTime.x * 1.1 + aParams.z +
                     (aPosScale.x + aPosScale.z * 0.7) * 0.05);
    world.xz += vec2(0.9, 0.35) * (gust * 0.07 * aUv.x * aPosScale.w * fade);

    gl_Position = uLightViewProj * vec4(world, 1.0);
}
