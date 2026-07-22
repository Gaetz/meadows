#version 460 core
#include "common.glsl"
#include "sky.glsl" // applyFog

// Placed water volume surface: a stylized, NON-mirrored sheet
// (the planar mirror belongs to the global sea alone). Sky-tinted fresnel
// + two scrolling ripple noise layers; alpha blend over the opaques.

layout(std140, binding = 1) uniform WaterVolumeUbo {
    vec4 uWaterTint; // rgb = linear water color, a = chop
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
    // Perturbed flat normal drives a stylized fresnel toward the sky tint.
    vec3 n = normalize(vec3((ripple - 0.5) * 0.35 * chop, 1.0,
                            (ripple - 0.5) * 0.28 * chop));
    vec3 view = normalize(uCameraPos.xyz - vWorldPos);
    float fresnel = pow(1.0 - clamp(dot(n, view), 0.0, 1.0), 3.0);
    vec3 sky = mix(uHorizonColor.rgb, uZenithColor.rgb, 0.6);
    vec3 water = uWaterTint.rgb * (uAmbientColor.rgb * 2.2 +
                                   uSunColor.rgb * 0.35);
    vec3 color = mix(water, sky, fresnel * 0.75);
    // Sparkle crests at high chop.
    color += uSunColor.rgb * smoothstep(0.78, 0.92, ripple) * 0.15 * chop;
    float alpha = mix(0.72, 0.9, fresnel);
    fragColor = vec4(applyFog(color, vWorldPos), alpha);
}
