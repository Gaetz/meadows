#version 460 core
#include "common.glsl"
#include "sky.glsl"

// Cliff-wall ribbons (docs/CLIFFS.md étage 2, CliffSystem). Shares the
// terrain pass's groups: same splat arrays, same shadow receivers, same
// vertex stage (terrain.vert — the ribbon mesh is world-space
// MeshVertex). The material is the CLIFF splat layer sampled
// TRIPLANARLY — a near-vertical wall under the terrain's planar mapping
// would stretch into taffy; here the wall and the surrounding ground
// share one texture family, so the seam at the foot/crest is a
// mapping change, not a material change.
layout(binding = 0) uniform sampler2DArray uSplat;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
layout(binding = 3) uniform sampler2DArray uSplatHeight;
layout(binding = 4) uniform sampler2D uTerrainShade0;
layout(binding = 5) uniform sampler2D uTerrainShade1;
layout(binding = 8) uniform sampler2DArray uSplatNormal;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"
#include "locallights.glsl"
#include "terrainlight.glsl"
#include "gi.glsl"

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in vec3 vWorldPos;

layout(location = 0) out vec4 fragColor;

const float kCliffLayer = 4.0;

void main() {
    vec3 n = normalize(vNormal);

    // Triplanar weights, sharpened so the three projections do not
    // ghost across each other on the faceted ribbon.
    vec3 bw = pow(abs(n) + 1.0e-5, vec3(3.0));
    bw /= bw.x + bw.y + bw.z;
    float tile = uTerrainInfo.z; // the terrain's tiles-per-meter
    vec2 uvx = vWorldPos.zy * tile;
    vec2 uvy = vWorldPos.xz * tile;
    vec2 uvz = vWorldPos.xy * tile;

    // BI-FREQUENCY: the near tile alone mips to a flat slab across a
    // valley (a 150 m wall at 300 m is one texel of the near
    // frequency). A macro octave at 0.09x carries the rock read to the
    // horizon; the near octave takes over as you approach.
    float camDist = distance(vWorldPos, uCameraPos.xyz);
    float macroW = clamp(camDist / 90.0, 0.25, 0.75);
    vec3 albedo = texture(uSplat, vec3(uvx, kCliffLayer)).rgb * bw.x +
                  texture(uSplat, vec3(uvy, kCliffLayer)).rgb * bw.y +
                  texture(uSplat, vec3(uvz, kCliffLayer)).rgb * bw.z;
    vec3 albedoMacro =
        texture(uSplat, vec3(uvx * 0.09, kCliffLayer)).rgb * bw.x +
        texture(uSplat, vec3(uvy * 0.09, kCliffLayer)).rgb * bw.y +
        texture(uSplat, vec3(uvz * 0.09, kCliffLayer)).rgb * bw.z;
    // LUMINANCE-only macro modulation: a direct color mix drifted the
    // far blocks beige while the steep ground stayed gray — the macro
    // octave carries the large-scale pattern, the near octave keeps
    // the hue.
    float lumNear = dot(albedo, vec3(0.299, 0.587, 0.114));
    float lumMacro = dot(albedoMacro, vec3(0.299, 0.587, 0.114));
    albedo *= mix(1.0,
                  clamp(lumMacro / max(lumNear, 1.0e-3), 0.55, 1.7),
                  macroW);
    float height =
        texture(uSplatHeight, vec3(uvx, kCliffLayer)).r * bw.x +
        texture(uSplatHeight, vec3(uvy, kCliffLayer)).r * bw.y +
        texture(uSplatHeight, vec3(uvz, kCliffLayer)).r * bw.z;
    // Anti-repetition (the terrain's non-harmonic luminance drift):
    // a second, incommensurate tap; two periods never realign.
    if (uSplatVarietyInfo.x > 0.0) {
        vec3 c2 = texture(uSplat,
                          vec3(uvy * 0.37 + vec2(0.23, 0.71),
                               kCliffLayer)).rgb;
        float l1 = dot(albedo, vec3(0.299, 0.587, 0.114));
        float l2 = dot(c2, vec3(0.299, 0.587, 0.114));
        float f = clamp(l2 / max(l1, 1e-3), 0.5, 1.8);
        albedo *= mix(1.0, f, uSplatVarietyInfo.x * 0.6);
    }

    // Per-plane tangent normals, whiteout-blended into the mesh normal
    // (axis frames per projection — the bark-path construction).
    // Normal detail rides the MACRO octave too — the near normals mip
    // away with their albedo, the macro ones keep the far face craggy.
    vec2 nx = mix(texture(uSplatNormal, vec3(uvx, kCliffLayer)).rg,
                  texture(uSplatNormal, vec3(uvx * 0.09,
                                             kCliffLayer)).rg,
                  macroW) * 2.0 - 1.0;
    vec2 ny = mix(texture(uSplatNormal, vec3(uvy, kCliffLayer)).rg,
                  texture(uSplatNormal, vec3(uvy * 0.09,
                                             kCliffLayer)).rg,
                  macroW) * 2.0 - 1.0;
    vec2 nz = mix(texture(uSplatNormal, vec3(uvz, kCliffLayer)).rg,
                  texture(uSplatNormal, vec3(uvz * 0.09,
                                             kCliffLayer)).rg,
                  macroW) * 2.0 - 1.0;
    vec3 shadedN = normalize(
        vec3(0.0, nx.y, nx.x) * (bw.x * sign(n.x)) +
        vec3(ny.x, 0.0, ny.y) * (bw.y * sign(n.y)) +
        vec3(nz.x, nz.y, 0.0) * (bw.z * sign(n.z)) + n * 1.6);

    // Mesh mask (strata tone + cavity, baked by CliffSystem) + the
    // terrain's macro tint so the wall follows the region's palette.
    albedo *= vColor;
    vec2 suv = (vWorldPos.xz - uTerrainShadeMapInfo.xy) *
                   uTerrainShadeMapInfo.z +
               0.5;
    if (uTerrainShadeMapInfo.w > 0.5 &&
        all(greaterThan(suv, vec2(0.0))) &&
        all(lessThan(suv, vec2(1.0)))) {
        albedo *= mix(vec3(1.0), texture(uTerrainShade0, suv).rgb,
                      uSplatDetailInfo.y);
    }
    albedo *= cascadeDebugTint(vWorldPos);
    albedo *= mix(1.0, 0.72, clamp(uStormInfo.y, 0.0, 1.0));

    // The terrain's lighting chain: stylized-aware sun, cloud + cascade
    // shadows, long-range terrain shadow, GI ambient, clustered locals.
    float ndl = dot(shadedN, uSunDirection.xyz);
    float diffuse = stylizedDiffuse(ndl, max(ndl, 0.0));
    float cloudVis = cloudShadowFactor(vWorldPos);
    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) * cloudVis;
    vec2 tl = terrainLightFactors(vWorldPos);
    vec3 lit = albedo * (giAmbient(vWorldPos, shadedN,
                                   uAmbientColor.rgb * tl.y) +
                         uSunColor.rgb * (diffuse * shadow * tl.x));
    if (uClusterInfo.x > 0.5) {
        lit += albedo * localLights(vWorldPos, shadedN);
    }
    // Alpha packs the relief height for the SSDM warp (terrain rule).
    fragColor = vec4(applyFog(lit, vWorldPos, cloudVis),
                     0.5 + clamp(height, 0.0, 1.0) * 0.49);
}
