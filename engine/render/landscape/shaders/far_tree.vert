#version 460 core
#include "compat.glsl"
#include "common.glsl"

// Far-tree impostors (FarTerrain): one cylindrical billboard per
// instance, corners from the vertex index (no per-vertex buffer). Fades
// in where the real trees end and out into the veil — the dissolve
// happens in the fragment (depth-write-friendly dither discard).
layout(location = 0) in vec4 aPosScale; // xyz ground point, w height (m)
layout(location = 1) in vec4 aParams;   // x crown seed, y tint jitter,
                                        // z width ratio, w trunk fraction

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec2 vUv; // x in [-0.5,0.5], y in [0,1]
layout(location = 2) out vec4 vParams;
layout(location = 3) out float vFade;

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
    // Width comes from the measured crown ratio of the real trees.
    vec3 p = ground + right * (c.x * h * abs(aParams.z)) +
             vec3(0.0, c.y * h, 0.0);
    vWorldPos = p;
    vUv = c;
    vParams = aParams;
    // In past the real-tree fade (uFogLayerInfo.w tracks the vegetation
    // ring), out into the fog veil.
    float fadeEnd = max(uFogLayerInfo.w, 100.0);
    vFade = smoothstep(fadeEnd * 0.8, fadeEnd + 20.0, d) *
            (1.0 - smoothstep(4200.0, 5200.0, d));
    gl_Position = uViewProj * vec4(p, 1.0);
}
