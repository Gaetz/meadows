#version 460 core
#include "compat.glsl"
#include "common.glsl"

// Chantier P0 C1 — CPU-simulated particles as camera-facing quads: six
// verts per instance from MEADOWS_VERTEX_INDEX (the rain pattern), instance data
// pulled from the FxInstances SSBO the FxRenderer refills per batch.

layout(std430, binding = 2) readonly buffer FxInstances {
    vec4 data[]; // pairs: [posSize, color] per particle
};

layout(location = 0) out vec2 vUv;    // -1..1 across the quad
layout(location = 1) out vec4 vColor;

void main() {
    int particle = MEADOWS_VERTEX_INDEX / 6;
    int corner = MEADOWS_VERTEX_INDEX % 6;
    vec4 posSize = data[particle * 2 + 0];
    vColor = data[particle * 2 + 1];

    // Spherical billboard: face the camera from anywhere (no view
    // matrix needed — uCameraPos is in the frame UBO).
    vec3 view = posSize.xyz - uCameraPos.xyz;
    float viewLen = length(view);
    view = viewLen > 1e-4 ? view / viewLen : vec3(0.0, 0.0, 1.0);
    vec3 up = abs(view.y) > 0.98 ? vec3(1.0, 0.0, 0.0)
                                 : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(view, up));
    vec3 realUp = cross(right, view);

    vec2 uv = (corner == 0) ? vec2(-1.0, -1.0)
              : (corner == 1) ? vec2(1.0, -1.0)
              : (corner == 2) ? vec2(1.0, 1.0)
              : (corner == 3) ? vec2(-1.0, -1.0)
              : (corner == 4) ? vec2(1.0, 1.0)
                              : vec2(-1.0, 1.0);
    vUv = uv;
    float halfSize = posSize.w * 0.5; // "half" is reserved in GLSL
    vec3 world = posSize.xyz + (right * uv.x + realUp * uv.y) * halfSize;
    gl_Position = uViewProj * vec4(world, 1.0);
}
