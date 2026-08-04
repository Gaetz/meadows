#version 460 core
#include "common.glsl"
#include "sky.glsl"

layout(binding = 0) uniform sampler2D uLeafMask;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
// Textured-prop normal map (flat 1x1 for untextured variants).
layout(binding = 3) uniform sampler2D uPropNormal;
// Region shading T0 (macro tint) — the ground-anchor blend below.
layout(binding = 4) uniform sampler2D uTerrainShade0;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"
#include "locallights.glsl"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in float vTint;
layout(location = 4) in vec2 vCardUv;
layout(location = 5) in vec2 vPropUv;
layout(location = 6) in float vGroundDelta;

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
    vec3 baseColor = vColor;
    if (vCardUv.x >= 0.0) {
        vec2 mask = texture(uLeafMask, vCardUv).ra;
        float lod = textureQueryLod(uLeafMask, vCardUv).x;
        float solid = smoothstep(uLeafLodInfo.x, uLeafLodInfo.y, lod);
        if (mix(mask.y, 1.0, solid) < 0.5) {
            discard;
        }
        leafShade = mix(mix(0.7, 1.3, mask.x), 1.0, solid);
        // Season: mix toward the slot's autumn tint, weighted by its
        // seasonality — evergreens stay green.
        vec4 season = uLeafSeason[int(floor(vCardUv.x * 8.0))];
        baseColor = mix(vColor, season.rgb, uSeasonInfo.x * season.a);
    }

    // Textured prop (docs/GRASS-REDO.md palier 2): group 1 holds the
    // variant's albedo instead of the leaf atlas; alpha cutout at 0.5
    // (opaque scans never discard — their alpha is solid 1). The normal
    // map perturbs through a derivative cotangent frame (no mesh
    // tangents needed — Schüler's trick).
    vec3 shadeN = normalize(vNormal);
    if (vPropUv.x > -5.0) {
        vec4 texel = texture(uLeafMask, vPropUv);
        if (texel.a < 0.5) {
            discard;
        }
        baseColor = texel.rgb * vColor;
        vec3 nTex = texture(uPropNormal, vPropUv).xyz * 2.0 - 1.0;
        vec3 dp1 = dFdx(vWorldPos);
        vec3 dp2 = dFdy(vWorldPos);
        vec2 duv1 = dFdx(vPropUv);
        vec2 duv2 = dFdy(vPropUv);
        vec3 dp2perp = cross(dp2, shadeN);
        vec3 dp1perp = cross(shadeN, dp1);
        vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
        float invmax =
            inversesqrt(max(dot(T, T), dot(B, B)) + 1.0e-10);
        shadeN = normalize(mat3(T * invmax, B * invmax, shadeN) * nTex);
    }

    // Per-instance hue roll: some trees lean yellow-green, some deep green.
    vec3 albedo = leafShade * baseColor *
                  mix(vec3(0.85, 1.0, 0.75), vec3(1.1, 1.0, 1.15), vTint);

    // Ground anchor (the Battlefront contract, docs/GRASS-REDO.md): the
    // base 0..0.4 m of every prop fades toward the terrain's macro tint —
    // rocks and trunks sit IN the ground instead of on it. Strength
    // follows the terrain's own tint knob (uSplatDetailInfo.y).
    vec2 anchorUv = (vWorldPos.xz - uTerrainShadeMapInfo.xy) *
                        uTerrainShadeMapInfo.z +
                    0.5;
    if (uTerrainShadeMapInfo.w > 0.5 &&
        all(greaterThan(anchorUv, vec2(0.0))) &&
        all(lessThan(anchorUv, vec2(1.0)))) {
        float anchor = (1.0 - smoothstep(0.05, 0.4, vGroundDelta)) *
                       max(uSplatDetailInfo.y, 0.35);
        albedo = mix(albedo,
                     albedo * texture(uTerrainShade0, anchorUv).rgb,
                     anchor);
    }

    albedo *= cascadeDebugTint(vWorldPos);
    vec3 n = shadeN;
    // Classic mode: wrap diffuse (soft-GI feel). Stylized mode: the shared
    // BotW step ramp — flat lit/shade plateaus over the faceted masses.
    float ndl = dot(n, uSunDirection.xyz);
    float wrap = clamp((ndl + 0.4) / 1.4, 0.0, 1.0);
    float diffuse = stylizedDiffuse(ndl, wrap);
    float cloudVis = cloudShadowFactor(vWorldPos);
    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) * cloudVis;
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

    fragColor = vec4(applyFog(lit, vWorldPos, cloudVis), 1.0);
}
