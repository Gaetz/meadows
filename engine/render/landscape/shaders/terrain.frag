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
// Per-layer tangent normals: rg = xy*0.5+0.5, z reconstructed (BC5 cooked
// or RGBA8 procedural — one decode path).
layout(binding = 8) uniform sampler2DArray uSplatNormal;
// Cooked ORM (r = AO, g = roughness): the surface response — AO on the
// ambient, roughness shaping the wet/snow sheen. The procedural
// fallback binds a placeholder here and zeroes uSurfSheenInfo.
layout(binding = 9) uniform sampler2DArray uSplatOrm;
#include "shadow.glsl"
#include "clouds.glsl"
#include "stylized.glsl"
#include "locallights.glsl"
#include "terrainlight.glsl"
#include "gi.glsl"
#include "terrain_weights.glsl"
#include "terrain_blend.glsl"
#include "terrain_zones.glsl"

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
    vec4 shade0 = shadeValid ? texture(uTerrainShade0, suv)
                             : vec4(1.0, 1.0, 1.0, 0.0);
    vec3 tint = shade0.rgb;
    // Baked ground wetness (rivers, lakes, the v4 incisions) — the
    // per-pixel half of what uStormInfo.y does globally.
    float wetMask = shadeValid ? shade0.a : 0.0;

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

    // Hex-tiling (terrain_zones.glsl): the grass layer is a 3-tap blend
    // over a triangle lattice — variant AND uv offset per vertex, so
    // neither zone borders nor tile repetition exist. Far away the
    // weights collapse to the dominant tap (1 fetch again).
    ivec2 hexV[3];
    vec3 hexW;
    hexGrass(vWorldPos.xz, hexV[0], hexV[1], hexV[2], hexW);
    vec2 hexOff[3];
    hexOff[0] = hexOffsetOf(hexV[0]);
    hexOff[1] = hexOffsetOf(hexV[1]);
    hexOff[2] = hexOffsetOf(hexV[2]);
    float camDistHex = distance(vWorldPos, uCameraPos.xyz);
    float hexFar = smoothstep(25.0, 40.0, camDistHex);
    // Collapse toward the dominant tap with distance (mip-blurred far
    // texels don't need the 3-way blend).
    int hexDom = hexW.y > hexW.x ? (hexW.z > hexW.y ? 2 : 1)
                                 : (hexW.z > hexW.x ? 2 : 0);
    vec3 hexOneHot = vec3(hexDom == 0 ? 1.0 : 0.0,
                          hexDom == 1 ? 1.0 : 0.0,
                          hexDom == 2 ? 1.0 : 0.0);
    hexW = mix(hexW, hexOneHot, hexFar);
    // Scree bias for the sand family's pick (terrain_weights.glsl
    // screeFactor — the same band that grew the sand apron).
    float screeMix =
        clamp(screeFactor(slope, vColor.r, wander) * 1.6, 0.0, 1.0);
    // Grass->snow transition = a two-stage DEPOSITION over the meadow
    // (the snow-on-rock recipe transposed): frost first, then white
    // snow, both composited per-pixel in the albedo loop with the
    // grass tile's own relief poking through and the fractal patch
    // field shaping tongues and bays — never a weight threshold (hard
    // cutouts) nor a per-vertex flip (hex lattice), both measured.
    float overlayBand =
        smoothstep(snowLine + snowOffset - 90.0,
                   snowLine + snowOffset + 60.0, h + wander * 26.0);
    float overlayPatch =
        overlayBand > 0.001 ? snowPatch01(vWorldPos.xz) : 0.5;
    // Subalpine heath band: approaching the snow line the heath grounds
    // (22/23) DEPOSIT per-pixel over the blended grass — same recipe as
    // the frost above it, NOT a per-vertex variant flip (an off-color
    // cell decision paints the hex lattice; measured twice: frost, then
    // this very heath). A coarser instance of the patch field gnaws the
    // adoption edge instead of tracing a level contour.
    float heathBand =
        smoothstep(snowLine + snowOffset - 260.0,
                   snowLine + snowOffset - 80.0, h + wander * 26.0);
    float heathPatch =
        heathBand > 0.001 ? snowPatch01(vWorldPos.xz * 0.31) : 0.5;
    // Steppe dry-grass band: the blended biome SANDINESS is the per-
    // pixel aridity signal (temperate 0 -> steppe -> arid): the
    // withered-grass ground (24) deposits over the green grass as the
    // climate dries, so the arid interior reads as dry steppe instead
    // of lush lawn. Same deposition recipe as the heath — never a
    // per-vertex variant flip (off-color cells paint the hex lattice).
    float dryBand = smoothstep(0.12, 0.55, shade1.b);
    float dryPatch =
        dryBand > 0.001 ? snowPatch01(vWorldPos.xz * 0.23 + 57.0) : 0.5;
    float grassLayerA = hexFamilyLayer(0, hexV[hexDom], 0.0);
    // Cliff variant panel (24 m) — every cliff fetch (height, albedo,
    // POM) must agree on it.
    float cliffLayer = hexFamilyLayer(4, hexV[hexDom], 0.0);

    // Height-blend the rule weights: only layers the rule already admits
    // fetch their displacement (2-3 typical), the winner's micro-relief
    // claims the transition band.
    float depth = uSplatDetailInfo.x;
    float hs[kSplatLayers];
    for (int i = 0; i < kSplatLayers; ++i) {
        if (ws[i] <= kSplatWeightEps) {
            hs[i] = 0.0;
            continue;
        }
        if (i <= 3) { // grass/rock/snow/sand: the per-family hex mix
            hs[i] = 0.0;
            for (int t = 0; t < 3; ++t) {
                if (hexW[t] > 0.003) {
                    hs[i] += hexW[t] *
                             texture(uSplatHeight,
                                     vec3(uv + hexOff[t],
                                          hexFamilyLayer(
                                              i, hexV[t],
                                              i == 3 ? screeMix
                                                     : 0.0))).r;
                }
            }
        } else {
            hs[i] = texture(uSplatHeight,
                            vec3(uv, i == 4 ? cliffLayer : float(i))).r;
        }
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

    int dominant = 0;
    float dominantB = 0.0;
    for (int i = 0; i < kSplatLayers; ++i) {
        if (b[i] > dominantB) {
            dominantB = b[i];
            dominant = i;
        }
    }

    // Tangent basis of the planar world mapping (u = +X, v = +Z): no
    // vertex tangents needed. B = cross(T, n) keeps +v => +Z (normal maps
    // are OpenGL +Y — a flipped-looking relief means the SOURCE map is DX).
    vec3 T = normalize(vec3(1.0, 0.0, 0.0) - n * n.x);
    vec3 B = cross(T, n);
    float camDist = distance(vWorldPos, uCameraPos.xyz);

    // Parallax occlusion on the DOMINANT layer (uSplatDetailInfo.w =
    // reach in meters, 0 = off), faded over its last stretch. Derivatives
    // are computed BEFORE the march and textureGrad used inside: implicit
    // derivatives are undefined in the non-uniform loop (mip bands +
    // shimmer at triangle edges otherwise).
    // The dominant layer's FETCH index follows the dominant hex tap —
    // its uv offset rides the whole POM march (subtracted back for the
    // other layers below). Every hex family (0-3) qualifies.
    float dominantLayer =
        dominant <= 3
            ? hexFamilyLayer(dominant, hexV[hexDom],
                             dominant == 3 ? screeMix : 0.0)
            : (dominant == 4 ? cliffLayer : float(dominant));
    vec2 pomShift = dominant <= 3 ? hexOff[hexDom] : vec2(0.0);
    vec2 pomUv = uv + pomShift;
    float pomSelfShadow = 1.0;
    float pomReach = uSplatDetailInfo.w;
    if (pomReach > 0.0 && camDist < pomReach && dominantB > 0.3 * total) {
        vec2 dx = dFdx(uv);
        vec2 dy = dFdy(uv);
        vec3 view = normalize(uCameraPos.xyz - vWorldPos);
        vec3 viewT = vec3(dot(view, T), dot(view, B), dot(view, n));
        float scale = uSplatVarietyInfo.z * // relief depth (panel knob)
                      (1.0 - smoothstep(pomReach * 0.6, pomReach, camDist));
        const int kPomSteps = 12;
        vec2 duv = viewT.xy / max(viewT.z, 0.25) * (scale / kPomSteps);
        float stepDepth = 1.0 / kPomSteps;
        float depthCur = 0.0;
        float hPrev = 0.0;
        float hHere =
            1.0 - textureGrad(uSplatHeight, vec3(pomUv, dominantLayer),
                              dx, dy).r;
        for (int s = 0; s < kPomSteps && depthCur < hHere; ++s) {
            pomUv -= duv;
            depthCur += stepDepth;
            hPrev = hHere;
            hHere = 1.0 -
                    textureGrad(uSplatHeight, vec3(pomUv, dominantLayer),
                                dx, dy).r;
        }
        // One linear refine between the last two samples.
        float after = hHere - depthCur;
        float before = hPrev - (depthCur - stepDepth);
        float t = clamp(before / max(before - after, 1e-4), 0.0, 1.0);
        pomUv += duv * (1.0 - t);

        // Cheap self-shadowing: two occlusion taps toward the sun in
        // tangent space — crevices darken against the light, doubling
        // the perceived depth for a fraction of the march cost.
        // Strength = uSplatVarietyInfo.y (0 = off).
        if (uSplatVarietyInfo.y > 0.0) {
            vec3 sunT = vec3(dot(uSunDirection.xyz, T),
                             dot(uSunDirection.xyz, B),
                             dot(uSunDirection.xyz, n));
            if (sunT.z > 0.1) {
                float hSurf =
                    textureGrad(uSplatHeight, vec3(pomUv, dominantLayer),
                                dx, dy).r;
                vec2 sdir = sunT.xy / sunT.z * scale;
                float occl = 0.0;
                for (int s = 1; s <= 2; ++s) {
                    float f = float(s) / 3.0;
                    float hTap = textureGrad(
                        uSplatHeight,
                        vec3(pomUv + sdir * f, dominantLayer), dx, dy).r;
                    occl += clamp((hTap - (hSurf + f * 0.75)) * 2.0, 0.0,
                                  0.35);
                }
                pomSelfShadow =
                    1.0 - occl * uSplatVarietyInfo.y;
            }
        }
    }

    vec3 albedo = vec3(0.0);
    vec2 nxy = vec2(0.0);
    // ORM rides the SAME weighted taps as the albedo (a single-tap
    // sample painted the hex lattice into the ambient — any per-pixel
    // term must blend exactly like the color it modulates).
    bool wantOrm =
        uSurfSheenInfo.x + uSurfSheenInfo.z + uSurfSheenInfo.w > 0.0;
    vec2 ormAcc = vec2(0.0);
    for (int i = 0; i < kSplatLayers; ++i) {
        if (b[i] <= 0.0) {
            continue;
        }
        // Families 0-3 are 3-tap hex blends (variant + offset per
        // lattice vertex); the cliff fetches its PANEL variant at the
        // unshifted uv.
        vec2 baseUv = pomUv - pomShift;
        float fetchLayer =
            i == 0 ? grassLayerA : (i == 4 ? cliffLayer : float(i));
        vec3 layer;
        vec2 layerN;
        bool overlayHere = i == 0 && overlayBand > 0.001;
        bool heathHere = i == 0 && heathBand > 0.001;
        bool dryHere = i == 0 && dryBand > 0.001;
        if (i <= 3) {
            layer = vec3(0.0);
            layerN = vec2(0.0);
            vec2 layerOrm = vec2(0.0);
            vec3 frostA = vec3(0.0);
            vec2 frostN = vec2(0.0);
            vec3 snowA = vec3(0.0);
            vec2 snowN = vec2(0.0);
            vec3 heathA = vec3(0.0);
            vec2 heathN = vec2(0.0);
            vec3 dryA = vec3(0.0);
            vec2 dryN = vec2(0.0);
            for (int t = 0; t < 3; ++t) {
                if (hexW[t] > 0.003) {
                    vec2 tapUv = baseUv + hexOff[t];
                    float lyr = hexFamilyLayer(
                        i, hexV[t], i == 3 ? screeMix : 0.0);
                    layer += hexW[t] *
                             texture(uSplat, vec3(tapUv, lyr)).rgb;
                    layerN += hexW[t] *
                              (texture(uSplatNormal,
                                       vec3(tapUv, lyr)).rg * 2.0 -
                               1.0);
                    if (wantOrm) {
                        layerOrm +=
                            hexW[t] *
                            texture(uSplatOrm, vec3(tapUv, lyr)).rg;
                    }
                    // Overlay taps ride the SAME hex offsets (their
                    // single-tap version re-exposed the 4 m tile grid
                    // as visible lines), at slightly detuned scales so
                    // nothing aligns with the grass tiles either.
                    // Deposit taps ride the SAME hex offsets as the
                    // grass (single-tap = visible 4 m grid, measured),
                    // at detuned scales so nothing aligns.
                    if (dryHere) {
                        dryA += hexW[t] *
                                texture(uSplat,
                                        vec3(tapUv * 0.77, 24.0)).rgb;
                        dryN +=
                            hexW[t] *
                            (texture(uSplatNormal,
                                     vec3(tapUv * 0.77, 24.0)).rg *
                                 2.0 -
                             1.0);
                    }
                    // Only the CHOICE between the two heaths is
                    // per-vertex — they share one chromatic family.
                    if (heathHere) {
                        float hl = heathLayerOf(hexV[t]);
                        heathA += hexW[t] *
                                  texture(uSplat,
                                          vec3(tapUv * 0.71, hl)).rgb;
                        heathN +=
                            hexW[t] *
                            (texture(uSplatNormal,
                                     vec3(tapUv * 0.71, hl)).rg *
                                 2.0 -
                             1.0);
                    }
                    if (overlayHere) {
                        frostA += hexW[t] *
                                  texture(uSplat,
                                          vec3(tapUv * 0.83, 21.0)).rgb;
                        frostN +=
                            hexW[t] *
                            (texture(uSplatNormal,
                                     vec3(tapUv * 0.83, 21.0)).rg *
                                 2.0 -
                             1.0);
                        snowA += hexW[t] *
                                 texture(uSplat,
                                         vec3(tapUv * 1.19, 2.0)).rgb;
                        snowN +=
                            hexW[t] *
                            (texture(uSplatNormal,
                                     vec3(tapUv * 1.19, 2.0)).rg *
                                 2.0 -
                             1.0);
                    }
                }
            }
            // The steppe dry grass is the climate's ground state: it
            // settles first, heath then frost then snow stack above.
            if (dryHere) {
                float dryRelief = hs[0] - 0.5;
                float dryCover = smoothstep(
                    0.08, 0.75,
                    dryBand * 1.05 - dryPatch * 0.45 - dryRelief * 0.3);
                layer = mix(layer, dryA, dryCover);
                layerN = mix(layerN, dryN, dryCover);
                if (wantOrm) {
                    layerOrm = mix(
                        layerOrm,
                        texture(uSplatOrm, vec3(baseUv, 24.0)).rg,
                        dryCover);
                }
            }
            // The subalpine heath settles under the frost: a feathered
            // per-pixel coverage shaped by its own coarser patch
            // instance — the transition-biome ground emerges in gnawed
            // pools, never in lattice cells.
            if (heathHere) {
                float heathRelief = hs[0] - 0.5;
                float heathCover = smoothstep(
                    0.08, 0.75,
                    heathBand * 1.1 - heathPatch * 0.5 -
                        heathRelief * 0.35);
                layer = mix(layer, heathA, heathCover);
                layerN = mix(layerN, heathN, heathCover);
                if (wantOrm) {
                    layerOrm = mix(
                        layerOrm,
                        texture(uSplatOrm, vec3(baseUv, 22.0)).rg,
                        heathCover);
                }
            }
            // Two-stage snow DEPOSITION over the blended grass: frost
            // (21) settles first, white snow (2) buries it — hollows
            // of the tile relief first, feathered coverage, tongues
            // and bays from the fractal patch field. The weight-level
            // snow only takes over ABOVE, once this is fully white.
            if (overlayHere) {
                float reliefBias = hs[0] - 0.5;
                float frostCover = smoothstep(
                    0.05, 0.6,
                    overlayBand * 1.15 - overlayPatch * 0.3 -
                        reliefBias * 0.55);
                float snowCover = smoothstep(
                    0.4, 0.95,
                    overlayBand * 1.25 - overlayPatch * 0.5 -
                        reliefBias * 0.8);
                layer = mix(mix(layer, frostA, frostCover), snowA,
                            snowCover);
                layerN = mix(mix(layerN, frostN, frostCover), snowN,
                             snowCover);
                if (wantOrm) {
                    // ORM stays a single tap: its low-frequency AO/
                    // roughness never shows the tile grid.
                    layerOrm = mix(
                        mix(layerOrm,
                            texture(uSplatOrm, vec3(baseUv, 21.0)).rg,
                            frostCover),
                        texture(uSplatOrm, vec3(baseUv, 2.0)).rg,
                        snowCover);
                }
            }
            ormAcc += layerOrm * b[i];
        } else {
            layer = texture(uSplat, vec3(baseUv, fetchLayer)).rgb;
            layerN = texture(uSplatNormal,
                             vec3(baseUv, fetchLayer)).rg * 2.0 - 1.0;
            if (wantOrm) {
                ormAcc += texture(uSplatOrm, vec3(baseUv, fetchLayer))
                              .rg *
                          b[i];
            }
        }
        // Anti-repetition (brief phase 5, cheap variant): a second tap at
        // a NON-HARMONIC frequency (0.37x, per-layer phase) drifts the
        // tile's luminance — two incommensurate periods never realign, so
        // the 4 m grid dissolves. The hex families skip it: their uv
        // offsets already de-tile; only the cliff still needs it.
        if (uSplatVarietyInfo.x > 0.0 && i > 3) {
            vec3 c2 = texture(uSplat,
                              vec3(baseUv * 0.37 +
                                       vec2(0.17, 0.29) * float(i),
                                   fetchLayer)).rgb;
            float l1 = dot(layer, vec3(0.299, 0.587, 0.114));
            float l2 = dot(c2, vec3(0.299, 0.587, 0.114));
            float f = clamp(l2 / max(l1, 1e-3), 0.5, 1.8);
            layer *= mix(1.0, f, uSplatVarietyInfo.x * 0.6);
        }
        if (i == 4) {
            layer *= 0.84 + 0.24 * ledge;
        }
        albedo += layer * b[i];
        nxy += layerN * b[i];
    }
    albedo /= total;
    nxy /= total;

    // Near-field detail: the dominant layer's normal re-sampled at a
    // NON-HARMONIC frequency (the regular grid of one frequency alone
    // never disappears), faded out by uSplatDetailInfo.z meters — an
    // unfaded detail normal aliases in the background.
    float detailFade = 1.0 - smoothstep(uSplatDetailInfo.z * 0.5,
                                        uSplatDetailInfo.z, camDist);
    if (detailFade > 0.0) {
        vec2 dxy = texture(uSplatNormal,
                           vec3(pomUv * 7.3, dominantLayer)).rg * 2.0 -
                   1.0;
        nxy += dxy * (0.5 * detailFade); // whiteout-style xy add
    }

    vec3 tsn = vec3(nxy, sqrt(max(1.0 - dot(nxy, nxy), 0.0)));
    vec3 shadedN = normalize(T * tsn.x + B * tsn.y + n * tsn.z);
    // Macro tint, attenuated by the strength knob (uSplatDetailInfo.y —
    // above ~0.4 the tint crushes the materials' own variation).
    albedo *= mix(vec3(1.0), tint, uSplatDetailInfo.y);

    albedo *= cascadeDebugTint(vWorldPos);
    // Wetness darkens the ground: the global rain term (roofs keep the
    // DROPS out via the occlusion map) combined with the baked
    // per-pixel mask — river banks and fresh incisions read damp.
    float wet = max(wetMask * uSurfSheenInfo.y,
                    clamp(uStormInfo.y, 0.0, 1.0));
    albedo *= mix(1.0, 0.72, wet);
    // Surface response from the weight-blended ORM accumulation.
    vec2 aoRough = vec2(1.0, 0.8);
    if (wantOrm) {
        vec2 orm = ormAcc / total;
        aoRough = vec2(mix(1.0, orm.x, uSurfSheenInfo.x), orm.y);
    }
    // The mapped normal drives the DIRECT terms only (sun diffuse, local
    // lights); shadow bias and GI keep the analytic normal — the RC
    // inject baked it, and the stylized shadow pools must not crawl with
    // texel-scale relief.
    float ndl = dot(shadedN, uSunDirection.xyz);
    float diffuse = stylizedDiffuse(ndl, max(ndl, 0.0));
    // Cast shadows quantize to flat pools; cloud shadows stay soft (they
    // drift — hard edges would crawl).
    float cloudVis = cloudShadowFactor(vWorldPos);
    float shadow = stylizedShadow(shadowFactor(vWorldPos, n)) * cloudVis *
                   pomSelfShadow;
    // Long-range terrain sun shadow (x) + sky openness (y).
    vec2 tl = terrainLightFactors(vWorldPos);
    // The ONE GI technique branch (gi.glsl) — Classic stays intact.
    // Per-material AO shapes the AMBIENT only: crevices deepen in the
    // shade, the stylized sun term keeps its flat readability.
    vec3 lit =
        albedo * (giAmbient(vWorldPos, n, uAmbientColor.rgb * tl.y) *
                      aoRough.x +
                  uSunColor.rgb * (diffuse * shadow * tl.x));
    // The wet/snow sheen — the terrain's ONLY specular, gated to wet
    // or snowy ground: incisions glisten, snow sparkles, dry land
    // stays pure stylized diffuse.
    float sheenAmt =
        wet * uSurfSheenInfo.z + (ws[2] / total) * uSurfSheenInfo.w;
    if (sheenAmt > 0.001) {
        vec3 viewDir = normalize(uCameraPos.xyz - vWorldPos);
        vec3 hv = normalize(uSunDirection.xyz + viewDir);
        float spec = pow(max(dot(shadedN, hv), 0.0),
                         mix(64.0, 8.0, aoRough.y)) *
                     (1.0 - 0.6 * aoRough.y);
        lit += uSunColor.rgb * spec * sheenAmt * shadow * tl.x;
    }
    // Direct local lights, CLUSTERED PATH ONLY (docs/RENDERING.md §5 B4):
    // the ground is fullscreen — the per-cluster list is what makes the
    // cost bearable. Off = the historical sun+GI-only terrain.
    if (uClusterInfo.x > 0.5) {
        lit += albedo * localLights(vWorldPos, shadedN);
    }
    // Alpha packs the blended relief height for the SSDM warp
    // (0.5 flat .. ~0.99 crest; the 0.5 floor keeps the grass-exempt
    // flag semantics of every screen pass).
    float relief = (b[0] * hs[0] + b[1] * hs[1] + b[2] * hs[2] +
                    b[3] * hs[3] + b[4] * hs[4]) /
                   total;
    fragColor = vec4(applyFog(lit, vWorldPos, cloudVis),
                     0.5 + clamp(relief, 0.0, 1.0) * 0.49);
}
