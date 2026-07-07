#version 460 core
#include "common.glsl"
#include "sky.glsl"

layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"
#include "terrainlight.glsl"

// Grass redo (chantier 7.8) — BotW palette: dark rooted base rising to a
// warm yellow-green tip, tips catching extra light when a wind wave
// passes (the reference's gust highlight). Terrain light map (33b/c) and
// rain wetness (31) fold in like the other surfaces.

in float vT;
in float vSide;
in float vTint;
in float vGust;
in vec3 vNormal;
in vec3 vWorldPos;

out vec4 fragColor;

void main() {
    // 7.8bis palette: MATCHED to the grass splat tile family (the meadow
    // must blend into the terrain it stands on — in BotW the field and
    // the ground are one color). Tips only mildly fresher; the gust
    // shimmer stays the accent.
    vec3 baseColor = mix(vec3(0.020, 0.052, 0.010),
                         vec3(0.032, 0.070, 0.014), vTint);
    vec3 tipColor = mix(vec3(0.070, 0.150, 0.032),
                        vec3(0.095, 0.170, 0.040), vTint);
    vec3 albedo = mix(baseColor, tipColor, vT * vT * (3.0 - 2.0 * vT));
    albedo += vec3(0.024, 0.024, 0.005) * (vGust * vT); // gust shimmer
    // Brick 31 wetness: rain darkens and cools the meadow.
    albedo *= mix(vec3(1.0), vec3(0.66, 0.72, 0.72),
                  clamp(uStormInfo.y, 0.0, 1.0));

    // Grounded look: gentle root occlusion (too dark reads as noise).
    float ao = mix(0.62, 1.0, vT);

    vec3 n = normalize(vNormal);
    // Classic mode: wrap diffuse (carpet-like). Stylized mode: the shared
    // BotW step ramp — the meadow becomes flat lit/shade fields.
    float ndl = dot(n, uSunDirection.xyz);
    float wrap = clamp((ndl + 0.5) / 1.5, 0.0, 1.0);
    float diffuse = stylizedDiffuse(ndl, wrap);

    // Backlight translucency (fake SSS) + a thin view-dependent sheen
    // along the blade, strongest near the tips.
    vec3 viewDir = normalize(vWorldPos - uCameraPos.xyz);
    float backlight = stylizedSss(vWorldPos) * 0.30 * vT;
    vec3 halfDir = normalize(uSunDirection.xyz - viewDir);
    float sheen = pow(max(dot(n, halfDir), 0.0), 24.0) * 0.18 * vT;

    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) *
                   cloudShadowFactor(vWorldPos);
    // 33b/c: distant terrain shadow + sky openness.
    vec2 tl = terrainLightFactors(vWorldPos);
    vec3 lit = albedo * ao *
                   (uAmbientColor.rgb * tl.y +
                    uSunColor.rgb *
                        ((diffuse + backlight) * shadow * tl.x)) +
               uSunColor.rgb * sheen * ao * shadow * tl.x;

    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
