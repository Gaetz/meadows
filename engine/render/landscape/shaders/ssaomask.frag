#version 460 core
#include "common.glsl"

// Depth unsharp-mask AO (2026-07-10, the BotW-friendly alternative to
// the sampled-hemisphere SSAO): compare each pixel's distance with the
// AVERAGE distance of a fixed screen-space disc around it — a crease or
// dip sits FURTHER than its neighbourhood and darkens, smoothly. This
// is a blur, not a Monte-Carlo estimate: no jitter, therefore no
// speckle, ever — the broad soft grounding the stepped-ramp look wants.
// Convex edges stay bright (center closer than neighbours = no term).
layout(binding = 0) uniform sampler2D uSceneDepth;

in vec2 vUv;
out vec4 fragColor;

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

void main() {
    float depth = texture(uSceneDepth, vUv).r;
    if (depth >= 0.99995) {
        fragColor = vec4(1.0); // sky
        return;
    }
    float center = distance(worldFromDepth(vUv, depth), uCameraPos.xyz);

    // Fixed golden-angle spiral, SCREEN-space radius: the footprint is
    // constant on screen (no altitude blow-up) and identical every
    // frame (no shimmer in motion).
    const int kTaps = 12;
    const float kRadiusPx = 14.0; // half-res texels
    vec2 texel = 1.0 / vec2(textureSize(uSceneDepth, 0));
    float sum = 0.0;
    float weight = 0.0;
    for (int i = 0; i < kTaps; ++i) {
        float t = (float(i) + 0.5) / float(kTaps);
        float angle = 2.3999632 * float(i); // golden angle
        vec2 uv = vUv + vec2(cos(angle), sin(angle)) *
                            (texel * (kRadiusPx * sqrt(t)));
        if (any(lessThan(uv, vec2(0.0))) ||
            any(greaterThan(uv, vec2(1.0)))) {
            continue;
        }
        float d = texture(uSceneDepth, uv).r;
        if (d >= 0.99995) {
            continue; // sky neighbours neither occlude nor brighten
        }
        float s = distance(worldFromDepth(uv, d), uCameraPos.xyz);
        // Range guard: geometry far in front/behind is DISCONNECTED
        // (silhouettes must not halo). Window scales with distance so
        // it stays meaningful at range.
        float range = 2.0 + center * 0.08;
        float w = 1.0 - smoothstep(range * 0.5, range * 2.0,
                                   abs(s - center));
        sum += s * w;
        weight += w;
    }
    if (weight < 0.5) {
        fragColor = vec4(1.0);
        return;
    }
    float avg = sum / weight;

    // Concavity term: how much deeper than the neighbourhood we sit.
    // The response window grows gently with distance (a 30 cm crease
    // matters up close, not at 400 m).
    float window = 0.9 + center * 0.015;
    float occlusion = smoothstep(0.05, window, center - avg);
    float ao = 1.0 - occlusion * 0.85;
    fragColor = vec4(ao, ao, ao, 1.0);
}
