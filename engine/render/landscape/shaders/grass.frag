#version 460 core
#include "common.glsl"
#include "sky.glsl"

layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"
#include "terrainlight.glsl"
#include "gi.glsl"

// Grass redo #2 (2026-07-11, dev-validated) — the Quick_Grass lighting
// model on our frame machinery (shadows, clouds, terrain light map,
// wetness, fog):
//  - WRAP DIFFUSE (wrap 0.5) — soft carpet lighting.
//  - BACKSCATTER — light bleeding THROUGH blades when looking toward the
//    sun (the reference's "backscatter fakery"), high-detail range only.
//  - ROOT AO — a deep occlusion ramp (easeIn^2) rising along the blade;
//    this is what makes the reference's meadow read dense.
//  - BLADE MIDDLE SHADING — the width-center darkens slightly, selling
//    the rounded cross-section together with the two blended normals.

layout(location = 0) in vec3 vColor;
layout(location = 1) in float vT;
layout(location = 2) in float vXSide;
layout(location = 3) in float vLodOut;
layout(location = 4) in vec3 vNormal1;
layout(location = 5) in vec3 vNormal2;
layout(location = 6) in vec3 vWorldPos;

layout(location = 0) out vec4 fragColor;

void main() {
    vec3 albedo = vColor;
    // Brick 31 wetness: rain darkens and cools the meadow (kept).
    albedo *= mix(vec3(1.0), vec3(0.66, 0.72, 0.72),
                  clamp(uStormInfo.y, 0.0, 1.0));

    // Blade middle darkening (near only — flattens out with the LOD).
    float acrossEdge = abs(vXSide * 2.0 - 1.0); // 0 center, 1 edges
    float middle = 1.0 - 0.15 * (1.0 - acrossEdge) * (1.0 - acrossEdge);
    albedo *= mix(middle, 1.0, vLodOut);

    // Deep root occlusion (the reference's density AO, easeIn^2 up).
    float ao = mix(0.30, 1.0, vT * vT);

    // The two rounded normals blend across the width.
    vec3 n = normalize(mix(vNormal1, vNormal2, vXSide));

    // Wrap diffuse; the shared stylized ramp keeps the BotW A/B working.
    float ndl = dot(n, uSunDirection.xyz);
    float wrap = clamp((ndl + 0.5) / 1.5, 0.0, 1.0);
    float diffuse = stylizedDiffuse(ndl, wrap);

    // Backscatter: view toward the sun lights the blade interior.
    vec3 viewDir = normalize(vWorldPos - uCameraPos.xyz);
    float backLight =
        clamp((dot(viewDir, uSunDirection.xyz) + 0.5) / 1.5, 0.0, 1.0);
    float scatter = backLight * 0.5 * (1.0 - vLodOut);

    // Thin sheen along the blade, strongest at the tips (kept).
    vec3 halfDir = normalize(uSunDirection.xyz - viewDir);
    float sheen = pow(max(dot(n, halfDir), 0.0), 32.0) * 0.25 * vT;

    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) *
                   cloudShadowFactor(vWorldPos);
    // 33b/c: distant terrain shadow + sky openness (kept).
    vec2 tl = terrainLightFactors(vWorldPos);
    // Chantier RC (G6): the ONE technique branch — Classic stays intact.
    vec3 lit = albedo * ao *
                   (giAmbient(vWorldPos, n, uAmbientColor.rgb * tl.y) +
                    uSunColor.rgb *
                        ((diffuse + scatter) * shadow * tl.x)) +
               uSunColor.rgb * sheen * ao * shadow * tl.x;

    fragColor = vec4(applyFog(lit, vWorldPos), 1.0);
}
