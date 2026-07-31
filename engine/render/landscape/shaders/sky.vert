#version 460 core
#include "compat.glsl"
#include "common.glsl"

// Fullscreen triangle from MEADOWS_VERTEX_INDEX (no vertex buffer), pinned to the far
// plane (reversed-Z: z = 0, w = 1) so GreaterEqual depth keeps it behind
// everything opaque.
layout(location = 0) out vec3 vRay;

void main() {
    vec2 uv = vec2((MEADOWS_VERTEX_INDEX << 1) & 2, MEADOWS_VERTEX_INDEX & 2);
    vec4 ndc = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    gl_Position = ndc;
    vec4 world = uInvViewProj * ndc;
    vRay = world.xyz / world.w - uCameraPos.xyz;
}
