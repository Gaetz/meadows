#version 460 core
#include "common.glsl"

// Fullscreen triangle from gl_VertexID (no vertex buffer), pinned to the far
// plane (z = w = 1) so LessEqual depth keeps it behind everything opaque.
out vec3 vRay;

void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vec4 ndc = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    gl_Position = ndc;
    vec4 world = uInvViewProj * ndc;
    vRay = world.xyz / world.w - uCameraPos.xyz;
}
