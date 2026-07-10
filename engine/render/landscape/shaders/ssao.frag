#version 460 core
#include "common.glsl"

// Half-res SSAO from the scene depth snapshot: world position reconstructed
// from depth, normal from its screen derivatives, 10 hemisphere samples
// rotated per pixel (IGN). Contact darkening under trees, rocks and along
// terrain creases — the "soft GI" grounding of the BotW look.
layout(binding = 0) uniform sampler2D uSceneDepth;

in vec2 vUv;
out vec4 fragColor;

vec3 worldFromDepth(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * ndc;
    return world.xyz / world.w;
}

// Fixed hemisphere kernel (z up), lengths staggered toward the center.
const vec3 kKernel[10] = vec3[](
    vec3( 0.21, -0.15, 0.30), vec3(-0.10,  0.24, 0.42),
    vec3( 0.35,  0.28, 0.25), vec3(-0.42, -0.09, 0.38),
    vec3( 0.06, -0.46, 0.52), vec3(-0.27,  0.51, 0.33),
    vec3( 0.58,  0.02, 0.45), vec3(-0.14, -0.62, 0.41),
    vec3( 0.44, -0.44, 0.64), vec3(-0.66,  0.30, 0.55));

void main() {
    float depth = texture(uSceneDepth, vUv).r;
    if (depth >= 0.99995) {
        fragColor = vec4(1.0); // sky: no occlusion
        return;
    }
    vec3 position = worldFromDepth(vUv, depth);
    vec3 normal = normalize(cross(dFdx(position), dFdy(position)));
    float dist = distance(position, uCameraPos.xyz);

    // Sample radius grows gently with distance so the effect stays a
    // similar on-screen size instead of shrinking to nothing — CAPPED
    // (speckle fix, 2026-07-10): unbounded growth spread the hemisphere
    // across depth discontinuities at altitude and peppered every
    // surface with black dots.
    float grow = min(dist, 120.0);
    float radius = 0.9 + grow * 0.02;
    float bias = 0.04 + grow * 0.004;

    // Per-pixel kernel rotation (IGN) trades banding for filterable noise.
    float angle = 6.2831853 *
                  fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                           0.00583715 * gl_FragCoord.y));
    float ca = cos(angle);
    float sa = sin(angle);
    vec3 up = abs(normal.y) < 0.9 ? vec3(0.0, 1.0, 0.0)
                                  : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);

    float occlusion = 0.0;
    for (int i = 0; i < 10; ++i) {
        vec3 k = kKernel[i];
        vec2 rotated = vec2(ca * k.x - sa * k.y, sa * k.x + ca * k.y);
        vec3 sampleDir =
            tangent * rotated.x + bitangent * rotated.y + normal * k.z;
        vec3 samplePoint = position + sampleDir * radius;

        vec4 clip = uViewProj * vec4(samplePoint, 1.0);
        if (clip.w <= 0.0) {
            continue;
        }
        vec2 sampleUv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(sampleUv, vec2(0.0))) ||
            any(greaterThan(sampleUv, vec2(1.0)))) {
            continue;
        }
        vec3 surface =
            worldFromDepth(sampleUv, texture(uSceneDepth, sampleUv).r);
        float surfaceDist = distance(surface, uCameraPos.xyz);
        float sampleDist = distance(samplePoint, uCameraPos.xyz);
        // Occluded when real geometry sits in front of the probe, unless it
        // is far outside the radius (range check kills distant silhouettes).
        // Smooth onset over the bias window (was a binary step — salt-and-
        // pepper noise the blur pass could not fully hide).
        float rangeFalloff =
            smoothstep(radius * 2.5, radius * 0.6, abs(surfaceDist - dist));
        occlusion += smoothstep(0.0, bias, sampleDist - surfaceDist - bias) *
                     rangeFalloff;
    }
    float ao = 1.0 - (occlusion / 10.0) * 0.95;
    fragColor = vec4(ao, ao, ao, 1.0);
}
