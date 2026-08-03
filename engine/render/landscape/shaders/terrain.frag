#version 460 core
#include "common.glsl"
#include "sky.glsl"

// Splat array, layers: 0 grass, 1 rock, 2 snow, 3 sand, 4 cliff
// (SplatTextures.hpp).
layout(binding = 0) uniform sampler2DArray uSplat;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
// Per-layer displacement heights (cooked R16 or procedural R16F).
// Binding 2 belongs to uCloudMap (clouds.glsl); 3 is the first free
// sampler slot in this shader's include closure.
layout(binding = 3) uniform sampler2DArray uSplatHeight;
// Region shading maps (TerrainShadeMap.hpp — encoding contract there):
// T0 = tint.rgb + wetness, T1 = rockiness / snow offset / sandiness / beach.
layout(binding = 4) uniform sampler2D uTerrainShade0;
layout(binding = 5) uniform sampler2D uTerrainShade1;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"
#include "locallights.glsl"
#include "terrainlight.glsl"
#include "gi.glsl"
#include "terrain_weights.glsl"
#include "terrain_blend.glsl"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec3 vWorldPos;

layout(location = 0) out vec4 fragColor;

void main() {
    vec3 n = normalize(vNormal);
    float h = vWorldPos.y;
    float slope = 1.0 - n.y;
    vec2 uv = vWorldPos.xz * uTerrainInfo.z;
    float seaLevel = uTerrainInfo.x;
    float snowLine = uTerrainInfo.y;

    // Rock claims slopes, snow the high flats, sand the shoreline band,
    // grass everything else (terrain_weights.glsl — the one weight rule).
    // Altitude borders are perturbed by borderWander (analytic noise,
    // material-set independent) so the sand and snow lines wander
    // organically instead of tracing a level contour.
    float wander = borderWander(uv * 0.06);

    // Region shading taps (biome rules resolved to continuous fields at
    // bake — the CPU mirror is terrain::regionShadingAt). Outside the
    // map's span the inputs fall back to neutral: the historical rules.
    vec2 suv = (vWorldPos.xz - uTerrainShadeMapInfo.xy) *
                   uTerrainShadeMapInfo.z +
               0.5;
    bool shadeValid = uTerrainShadeMapInfo.w > 0.5 &&
                      all(greaterThan(suv, vec2(0.0))) &&
                      all(lessThan(suv, vec2(1.0)));
    vec4 shade1 = shadeValid ? texture(uTerrainShade1, suv)
                             : vec4(0.0, 128.0 / 255.0, 0.0, 0.0);
    float rockShift = 0.1 * shade1.r;
    float snowOffset = (shade1.g * 255.0 - 128.0) * 8.0;
    vec3 tint = shadeValid ? texture(uTerrainShade0, suv).rgb : vec3(1.0);

    // vColor.r carries the baked rock-exposure mask (TerrainSystem
    // vertex build) — bare cliff faces claim the steepest slopes.
    TerrainWeights w =
        terrainWeights(h, slope, wander, vColor.r, seaLevel,
                       snowLine + snowOffset, rockShift, shade1.b,
                       shade1.a);
    float ws[kSplatLayers];
    ws[0] = w.grass;
    ws[1] = w.rock;
    ws[2] = w.snow;
    ws[3] = w.sand;
    ws[4] = w.cliff;

    // Height-blend the rule weights: only layers the rule already admits
    // fetch their displacement (2-3 typical), the winner's micro-relief
    // claims the transition band.
    float depth = uSplatDetailInfo.x;
    float hs[kSplatLayers];
    for (int i = 0; i < kSplatLayers; ++i) {
        hs[i] = ws[i] > kSplatWeightEps
                    ? texture(uSplatHeight, vec3(uv, float(i))).r
                    : 0.0;
    }
    float b[kSplatLayers];
    float total;
    if (depth > 0.0) {
        total = blendHeights(hs, ws, depth, b);
    } else {
        for (int i = 0; i < kSplatLayers; ++i) {
            b[i] = ws[i];
        }
        total = max(ws[0] + ws[1] + ws[2] + ws[3] + ws[4], 1.0e-5);
    }

    // Altitude-locked strata ledges on the cliff layer (the texture's
    // own banding runs in uv space; this one follows the geology).
    float band = fract((h + wander * 8.0) / 14.0);
    float ledge = smoothstep(0.0, 0.45, band) * (1.0 - smoothstep(0.7, 0.95, band));

    vec3 albedo = vec3(0.0);
    for (int i = 0; i < kSplatLayers; ++i) {
        if (b[i] <= 0.0) {
            continue;
        }
        vec3 layer = texture(uSplat, vec3(uv, float(i))).rgb;
        if (i == 4) {
            layer *= 0.84 + 0.24 * ledge;
        }
        albedo += layer * b[i];
    }
    albedo /= total;
    // Macro tint, attenuated by the strength knob (uSplatDetailInfo.y —
    // above ~0.4 the tint crushes the materials' own variation).
    albedo *= mix(vec3(1.0), tint, uSplatDetailInfo.y);

    albedo *= cascadeDebugTint(vWorldPos);
    // Wetness: rain darkens the ground (global for now — the roof
    // keeps the DROPS out via the occlusion map; per-pixel dry patches
    // under cover are a later refinement).
    albedo *= mix(1.0, 0.72, clamp(uStormInfo.y, 0.0, 1.0));
    float ndl = dot(n, uSunDirection.xyz);
    float diffuse = stylizedDiffuse(ndl, max(ndl, 0.0));
    // Cast shadows quantize to flat pools; cloud shadows stay soft (they
    // drift — hard edges would crawl).
    float cloudVis = cloudShadowFactor(vWorldPos);
    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) * cloudVis;
    // Long-range terrain sun shadow (x) + sky openness (y).
    vec2 tl = terrainLightFactors(vWorldPos);
    // The ONE GI technique branch (gi.glsl) — Classic stays intact.
    vec3 lit = albedo * (giAmbient(vWorldPos, n, uAmbientColor.rgb * tl.y) +
                         uSunColor.rgb * (diffuse * shadow * tl.x));
    // Direct local lights, CLUSTERED PATH ONLY (docs/RENDERING.md §5 B4):
    // the ground is fullscreen — the per-cluster list is what makes the
    // cost bearable. Off = the historical sun+GI-only terrain.
    if (uClusterInfo.x > 0.5) {
        lit += albedo * localLights(vWorldPos, n);
    }
    fragColor = vec4(applyFog(lit, vWorldPos, cloudVis), 1.0);
}
