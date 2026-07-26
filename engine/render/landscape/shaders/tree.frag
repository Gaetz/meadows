#version 460 core
#include "common.glsl"
#include "sky.glsl"

layout(binding = 0) uniform sampler2D uLeafMask;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"
#include "locallights.glsl"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in float vTint;
layout(location = 4) in vec2 vCardUv;

layout(location = 0) out vec4 fragColor;

void main() {
    // Leaf cards: cutout through the shared leaf-cluster mask, plus a
    // per-leaf shade roll (r channel) so a card reads as many leaves.
    // Mip averaging thins alpha-tested coverage until distant foliage
    // evaporates, so the mask ramps toward SOLID as the card's on-screen
    // footprint shrinks: near = crisp leaves, mid = thickened leaves,
    // far = the full stylized canopy mass (per-leaf shade off — noise at
    // that size). Mip-driven, so tree scale is accounted for free.
    float leafShade = 1.0;
    if (vCardUv.x >= 0.0) {
        vec2 mask = texture(uLeafMask, vCardUv).ra;
        float lod = textureQueryLod(uLeafMask, vCardUv).x;
        float solid = smoothstep(uLeafLodInfo.x, uLeafLodInfo.y, lod);
        if (mix(mask.y, 1.0, solid) < 0.5) {
            discard;
        }
        leafShade = mix(mix(0.7, 1.3, mask.x), 1.0, solid);
    }

    // Per-instance hue roll: some trees lean yellow-green, some deep green.
    vec3 albedo = leafShade * vColor *
                  mix(vec3(0.85, 1.0, 0.75), vec3(1.1, 1.0, 1.15), vTint);

    albedo *= cascadeDebugTint(vWorldPos);
    vec3 n = normalize(vNormal);
    // Classic mode: wrap diffuse (soft-GI feel). Stylized mode: the shared
    // BotW step ramp — flat lit/shade plateaus over the faceted masses.
    float ndl = dot(n, uSunDirection.xyz);
    float wrap = clamp((ndl + 0.4) / 1.4, 0.0, 1.0);
    float diffuse = stylizedDiffuse(ndl, wrap);
    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) *
                   cloudShadowFactor(vWorldPos);
    vec3 lit =
        albedo * (uAmbientColor.rgb + uSunColor.rgb * (diffuse * shadow));
    // Stepped rim against the sky — canopies pop off the
    // background (moved here from the removed leaf-card pass).
    lit += albedo * stylizedRim(n, vWorldPos) * uSunColor.rgb * shadow;
    // Direct local lights, CLUSTERED PATH ONLY (docs/RENDERING.md §5 B4):
    // trunks and canopies catch the torch below them. The reflection
    // pass leaves the flag off and skips this for free.
    if (uClusterInfo.x > 0.5) {
        lit += albedo * localLights(vWorldPos, n);
    }

    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
