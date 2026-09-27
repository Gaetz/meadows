#version 460 core
#include "common.glsl"
#include "sky.glsl" // applyFog

// Placed water volume surface: a stylized, NON-mirrored sheet
// (the planar mirror belongs to the global sea alone). Sky-tinted fresnel
// + two scrolling ripple noise layers; alpha blend over the opaques.

layout(std140, binding = 1) uniform WaterVolumeUbo {
    vec4 uWaterTint;     // rgb = linear water color, a = chop
    vec4 uWaterMeshInfo; // x = 1: a transient BODY (blob, jet) — shade
                         // with the geometric normal, not the flat sheet
};

layout(location = 0) in vec2 vUv;       // world XZ / 4 (ripple space)
layout(location = 1) in vec3 vWorldPos;
layout(location = 0) out vec4 fragColor;

float hash21(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 s = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), s.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), s.x),
               s.y);
}

void main() {
    float chop = uWaterTint.a;
    float t = uWindInfo.x;
    float ripple =
        vnoise(vUv * 3.0 + vec2(t * 0.20, t * 0.13)) * 0.6 +
        vnoise(vUv * 7.0 - vec2(t * 0.31, t * 0.09)) * 0.4;
    vec3 view = normalize(uCameraPos.xyz - vWorldPos);
    // Perturbed flat normal drives a stylized fresnel toward the sky tint;
    // a transient body (a carried blob, a jet's arc) uses its faceted
    // geometric normal instead — the low-poly look reads as water in
    // motion without any per-vertex data.
    vec3 n;
    if (uWaterMeshInfo.x > 0.5) {
        vec3 gn = normalize(cross(dFdx(vWorldPos), dFdy(vWorldPos)));
        if (dot(gn, view) < 0.0) {
            gn = -gn;
        }
        n = normalize(gn + vec3((ripple - 0.5) * 0.25 * chop,
                                (ripple - 0.5) * 0.15 * chop,
                                (ripple - 0.5) * 0.25 * chop));
    } else {
        n = normalize(vec3((ripple - 0.5) * 0.35 * chop, 1.0,
                           (ripple - 0.5) * 0.28 * chop));
    }
    float fresnel = pow(1.0 - clamp(dot(n, view), 0.0, 1.0), 3.0);
    vec3 sky = mix(uHorizonColor.rgb, uZenithColor.rgb, 0.6);
    vec3 water = uWaterTint.rgb * (uAmbientColor.rgb * 2.2 +
                                   uSunColor.rgb * 0.35);
    vec3 color = mix(water, sky, fresnel * 0.75);
    // Sparkle crests at high chop.
    color += uSunColor.rgb * smoothstep(0.78, 0.92, ripple) * 0.15 * chop;
    float alpha = mix(0.72, 0.9, fresnel);
    if (uWaterMeshInfo.x > 0.5) {
        alpha = mix(0.8, 0.95, fresnel); // a body is denser than a sheet
    }
    fragColor = vec4(applyFog(color, vWorldPos), alpha);
}
