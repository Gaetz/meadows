#version 460 core
#include "common.glsl"
#include "sky.glsl"

layout(binding = 0) uniform sampler2D uLeafMask;
layout(binding = 1) uniform sampler2DArrayShadow uShadowMap;
// Vegetation-only samplers live on slots 12-14: the main pass keeps
// PASS-LEVEL groups resident (2 = cloud map, 4/5 = shade maps, 6 = key
// shadow, 7 = terrain light map, 11 = GI) — a veg group on those slots
// either reads them as its own texture (bark showed the light map's
// sky-aperture whites, re-baked with the sun: the "clouds on trunks")
// or stomps them for every later draw in the pass.
layout(binding = 12) uniform sampler2D uPropNormal;
// Per-tree-slot bark (oak/spruce — the tree builder's pick), sampled
// triplanarly on flagged wood. uSplatVarietyInfo.w gates it (0 until
// the scene loaded the textures). uBarkNrm packs nor_gl in RGB and the
// displacement in A — the LOW-POLY trunk carries its relief here.
layout(binding = 13) uniform sampler2D uBark;
layout(binding = 14) uniform sampler2D uBarkNrm;
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
layout(location = 7) in vec4 vObjPos;
layout(location = 8) in vec3 vObjNormal;

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

    // SSDM relief packed in alpha. NEUTRAL is 0.745 — the decode
    // centers at mid height ((a-0.5)*2 - 0.5): packing "flat" at 0.5
    // decoded as a FULL PIT and every neutral prop dug itself in by
    // amp/2 (the "ça rentre" bug). Bark and disp-mapped props write
    // their real height.
    float reliefA = 0.745;
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
        vec4 texN = texture(uPropNormal, vPropUv);
        // Scanned props with a merged displacement carry their SSDM
        // height in the normal's alpha (255 = none — the guard).
        if (texN.a < 0.995) {
            reliefA = 0.5 + texN.a * 0.49;
        }
        vec3 nTex = texN.xyz * 2.0 - 1.0;
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

    // Bark on flagged wood (procedural trunks/branches): triplanar over
    // the object-space position — no mesh uvs, no seams on bent
    // branches. The RELIEF lives in the material (low-poly trunk):
    // parallax bump-offset shifts the taps along the view, the packed
    // normal map perturbs the shading normal (whiteout blend), and the
    // vertex color keeps carrying AO + the vertical gradient.
    if (vObjPos.w > 0.5 && uSplatVarietyInfo.w > 0.5) {
        float yaw = vObjPos.w - 1.0;
        float yc = cos(yaw);
        float ys = sin(yaw);
        vec3 onrm = normalize(vObjNormal);
        vec3 an = abs(onrm) + 1.0e-5;
        vec3 bw = an / (an.x + an.y + an.z);
        // Coarser than the first pass (0.6): at fine tilings the
        // displacement mips to mid-gray and the SSDM warp flattens —
        // the boulder-scale features are what read (retour dev).
        const float kBarkTile = 0.1; // tiles per meter (hand-tuned)
        const float kBarkDepth = 0.045; // parallax amplitude (uv units)
        vec2 uvx = vObjPos.zy * kBarkTile;
        vec2 uvy = vObjPos.xz * kBarkTile;
        vec2 uvz = vObjPos.xy * kBarkTile;
        // View in OBJECT space (inverse of the vertex yaw rotation).
        vec3 vw = normalize(uCameraPos.xyz - vWorldPos);
        vec3 vo = vec3(vw.x * yc + vw.z * ys, vw.y,
                       -vw.x * ys + vw.z * yc);
        // Normal-height taps (unshifted): the height drives the offset,
        // the normals perturb through per-plane axis frames.
        vec4 nhx = texture(uBarkNrm, uvx);
        vec4 nhy = texture(uBarkNrm, uvy);
        vec4 nhz = texture(uBarkNrm, uvz);
        float height =
            nhx.a * bw.x + nhy.a * bw.y + nhz.a * bw.z;
        float sink = (height - 0.5) * kBarkDepth;
        uvx += vec2(vo.z, vo.y) / max(abs(vo.x), 0.35) * sink;
        uvy += vec2(vo.x, vo.z) / max(abs(vo.y), 0.35) * sink;
        uvz += vec2(vo.x, vo.y) / max(abs(vo.z), 0.35) * sink;
        vec3 bark = texture(uBark, uvx).rgb * bw.x +
                    texture(uBark, uvy).rgb * bw.y +
                    texture(uBark, uvz).rgb * bw.z;
        baseColor = bark * min(vColor * 2.6, vec3(1.4));
        // Triplanar normal blend (axis frames per plane), rotated back
        // to world by the instance yaw.
        vec3 tnx = nhx.xyz * 2.0 - 1.0;
        vec3 tny = nhy.xyz * 2.0 - 1.0;
        vec3 tnz = nhz.xyz * 2.0 - 1.0;
        vec3 nObj = normalize(
            vec3(tnx.z * sign(onrm.x), tnx.y, tnx.x) * bw.x +
            vec3(tny.x, tny.z * sign(onrm.y), tny.y) * bw.y +
            vec3(tnz.x, tnz.y, tnz.z * sign(onrm.z)) * bw.z +
            onrm * 1.5);
        shadeN = normalize(vec3(nObj.x * yc - nObj.z * ys, nObj.y,
                                nObj.x * ys + nObj.z * yc));
        reliefA = 0.5 + clamp(height, 0.0, 1.0) * 0.49;
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

    fragColor = vec4(applyFog(lit, vWorldPos, cloudVis), reliefA);
}
