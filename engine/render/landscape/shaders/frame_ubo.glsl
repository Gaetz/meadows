// GENERATED from engine/render/landscape/FrameLanes.hpp by
// render::frameUboGlsl() - do not edit by hand. tests/FrameLanesTest.cpp
// pins this file to the generator and prints the regenerated text on a
// mismatch: paste it here.
#ifndef MEADOWS_FRAME_UBO_GLSL
#define MEADOWS_FRAME_UBO_GLSL

// Constants shared with the C++ side (MEADOWS_FRAME_CONSTANTS); each one
// is read from the declaration that owns it.
// Light-cluster grid cells along X (LightClusters, clusters.glsl).
const int kClusterX = 16;
// Light-cluster grid cells along Y.
const int kClusterY = 9;
// Light-cluster depth slices - shared with the froxel-fog grid.
const int kClusterZ = 64;
// Per-cluster list capacity: slot 0 = light count, the rest indices.
const int kClusterSlots = 32;
// LightsUbo array capacity (FrameDecisions.hpp LightsUniforms).
const int kMaxLights = 64;
// Legacy per-pixel light loop cap when the clustered path is off.
const int kFallbackLights = 24;
// RcUbo light array capacity - the GI takes the nearest lights.
const int kRcMaxLights = 24;
// WaterMaterialsUbo table size (water_surface.glsl).
const int kMaxWaterMaterials = 16;
// Rows of the grass species tables (GrassSpecies.hpp).
const int kGrassSpeciesCount = 6;
// MistMap R quantization: mist top = seaLevel + offset + r * range.
const float kMistTopOffset = -64.0;
// MistMap R quantization range (m).
const float kMistTopRange = 512.0;
// Inner far ring's impostor reach (m) - far_tree.vert fades out over
// the last 1000 m; the bake stops scattering impostors there.
const float kFarTreeFar = 5200.0;
// Sea sheet half-span floor (m): half the inner far ring's span.
const float kSeaQuadHalfSpan = 9000.0;

// Per-frame constants shared by every landscape shader (MEADOWS_FRAME_LANES
// - render::FrameUniforms, uploaded once per frame). std140: every lane
// vec4- or mat4-sized. Edit the table, never this block.
layout(std140, binding = 0) uniform FrameUbo {
    // World -> clip for this view.
    mat4 uViewProj;
    // NDC -> world, for fullscreen ray reconstruction.
    mat4 uInvViewProj;
    // xyz = camera world position, w = 1.
    vec4 uCameraPos;
    // x = seconds since scene start, y = free,
    // z = volumetric shaft intensity (0 indoors unless the froxel fog
    // runs), w = debug buffer view index (tonemap.frag).
    vec4 uTime;
    // xyz = normalized, points TOWARD the sun; w = frame dt (s) for the
    // auto-exposure adaptation (adapt.frag).
    vec4 uSunDirection;
    // rgb = sun light color (black indoors), w = sun disc intensity.
    vec4 uSunColor;
    // rgb = sky halo/afterglow (outlives the disc; black indoors),
    // w = grade vibrance (0 = neutral).
    vec4 uSunGlowColor;
    // rgb = sky ambient term (indoors: the room ambient, daylight-
    // coupled), w = stylized-lighting blend (0 classic wrap, 1 BotW step
    // ramp - stylized.glsl).
    vec4 uAmbientColor;
    // rgb = sky gradient top, w = grade split-tone (0 = neutral).
    vec4 uZenithColor;
    // rgb = horizon on the SUN side (warm at dusk), w = grade contrast
    // (1 = neutral).
    vec4 uHorizonColor;
    // rgb = horizon opposite the sun (already night), w = auto-exposure
    // minimum.
    vec4 uHorizonFarColor;
    // x = sea level, y = snow line (m), z = splat UV scale (tiles/meter),
    // w = planar reflections active (0/1).
    vec4 uTerrainInfo;
    // x = filmic tonemap (0/1), y = exposure, z = cascade debug view
    // (0/1), w = bloom intensity.
    vec4 uPostInfo;
    // x = density, y = height falloff (1/m), z = low-altitude boost,
    // w = start distance (m). Indoors: {0, 0.02, 0, 1e5} (no fog).
    vec4 uFogInfo;
    // World -> cascade shadow clip space, one per cascade.
    mat4 uSunViewProj[3];
    // xyz = cascade far view-distances (m), w = free.
    vec4 uCascadeSplits;
    // xyz = world texel size per cascade, w = shadow strength
    // (0 = shadows off).
    vec4 uShadowInfo;
    // xy = viewport size (px), zw = 1/size.
    vec4 uScreenInfo;
    // x = coverage [0,1], y = layer height (m), z = pattern scale (1/m),
    // w = shadow strength.
    vec4 uCloudInfo;
    // xy = sun position in screen UV, z = shaft visibility fade,
    // w = god-ray intensity (all neutral indoors).
    vec4 uSunScreen;
    // xy = baked cloud-field center (world XZ, snapped to the map cell),
    // z = 1/span, w = auto-exposure maximum.
    vec4 uCloudMapInfo;
    // xy = pool-depth map center (world XZ), z = 1/span, w unused.
    vec4 uWaterMapInfo;
    // x = accumulated wind time (s, speed-scaled - keeps cloud/wave phase
    // continuous when the weather changes the wind), y = sway strength,
    // z = water chop multiplier, w = auto-exposure active (0/1).
    vec4 uWindInfo;
    // Terrain light map (far terrain shadows + sky aperture): xy = map
    // center (world XZ), z = 1/span, w = strength (0 = feature off: no
    // bake yet, toggled off, or indoors).
    vec4 uTerrainLightInfo;
    // x = the EFFECTIVE water surface Y above the camera (sea level
    // outdoors, a volume's top when inside one, -1e6 = dry) - the tonemap
    // submersion reads this, not the sea level. yzw unused.
    vec4 uSubmersionInfo;
    // x = storm front 0-1 (horizon cumulonimbus), y = rain intensity 0-1
    // (streaks + wetness + the occlusion matrix; 0 indoors), zw unused.
    vec4 uStormInfo;
    // World -> top-down ortho clip for the rain occlusion depth (no rain
    // under roofs; 40 m around the camera). Zero when it does not rain.
    mat4 uRainOcclusionViewProj;
    // Interactive grass bending: xy = the player's feet XZ, z = feet Y,
    // w = bend radius (0 = off, e.g. Fly mode).
    vec4 uGrassBendInfo;
    // Meadow tuning, live from the render panel's "Grass" category
    // (GrassRenderTuning): x = blade height (m), y = half width (m),
    // zw = high-detail near/far (m).
    vec4 uGrassShapeInfo;
    // xy = density thinning start/end (m), z = far density floor,
    // w = width compensation at far density (wider blades, same mass).
    vec4 uGrassLodInfo;
    // rgb = base tint x root ground albedo, w = distance fade start (m).
    vec4 uGrassBaseTint;
    // rgb = tip tint x root ground albedo, w = distance fade end (m).
    vec4 uGrassTipTint;
    // The GiTechnique switch (gi.glsl): x = technique (0 classic,
    // 1 radiance cascades - surface shaders swap their ambient term for
    // the merged cascade-0 sample), y = intensity, z = edge fade width
    // (m), w = grid resolution. Resolved only: the reflection pass stays
    // classic, byte-identical exterior.
    vec4 uGiInfo;
    // xyz = cascade-0 grid origin, w = probe spacing.
    vec4 uGiGridInfo;
    // GI ramp: x = band count between floor and classic ambient
    // (0 = smooth), y = anti-aliasing width across a band edge, z = GI
    // ambient floor (fraction of classic - grid-border seam killer),
    // w unused.
    vec4 uGiBandInfo;
    // Foliage-card leaf cutout -> solid ramp (tree.frag/shadow_prop.frag):
    // xy = mip window start/end of the blend. z = mirror-pass flag (the
    // planar reflection sets 1): billboard cards re-aim at the mirrored
    // camera, so their winding does NOT flip with the pass's inverted
    // front face - tree.vert flips the card corners instead. w unused.
    vec4 uLeafLodInfo;
    // Fog sun single-scatter: x = strength (weather-crossfaded), y = phase
    // exponent, z = volumetric march reach (m) - the analytic fog of the
    // surfaces starts where the march ends (froxel reach, or 1400 for the
    // 2D march; 0 = no march, full analytic fog: reflection pass, night),
    // w = interior dust density (froxel fog indoors).
    vec4 uFogSunInfo;
    // Stylized diffuse ramp (stylized.glsl): xy = terminator smoothstep
    // edges, zw = full-light smoothstep edges.
    vec4 uStylizedDiffuseInfo;
    // Stylized shadow snap: xy = window edges, z = shadow floor,
    // w = half-tone level of the diffuse ramp.
    vec4 uStylizedShadowInfo;
    // Clustered forward (docs/RENDERING.md sec. 5): x = clustered path active
    // (0 = legacy nearest-lights loop), y = cluster grid far reach (m) -
    // the froxel slicing's far, so both grids share their z slices.
    // zw unused. Resolved only: the reflection pass keeps the legacy loop.
    vec4 uClusterInfo;
    // Key-shadow ATLAS (docs/RENDERING.md sec. 5): world -> tile clip per
    // slot - up to 4 shadowed lights, one 1024^2 tile each in the 2048^2
    // key-shadow target. A light finds its slot through LightsUbo
    // windowInfo.z (slot+1, 0 = unshadowed). Resolved only.
    mat4 uKeyShadowAtlasViewProj[4];
    // Grass shading knobs (grass.vert/.frag): x = root-AO floor
    // (1 = none), y = tip sheen strength, z = near blade-normal share
    // (0 = blades light like the ground), w unused.
    vec4 uGrassShadeInfo;
    // xy = whole-blade brightness hash range (min/max), z = blade middle
    // darkening, w = backscatter strength.
    vec4 uGrassBladeInfo;
    // Stylized specular band on characters/props (mesh/skinned.frag):
    // x = strength, y = band threshold (lower = wider highlight),
    // z = Blinn-Phong exponent, w unused.
    vec4 uStylizedSpecInfo;
    // Ground mist (mist.frag, docs/RENDERING.md sec. 3.5): x = extinction
    // sigma (1/m, weather-crossfaded; 0 = feature off - THE shader gate),
    // y = reach (m), z = coverage threshold, w = coverage softness.
    // Resolved only, exterior only.
    vec4 uMistInfo;
    // x = coverage pattern scale (1/m), y = erosion noise scale (1/m),
    // z = erosion strength, w = lift (m, raises the baked mist top).
    vec4 uMistShapeInfo;
    // Mist valley map (MistMap): xy = bake center (world XZ), z = 1/span,
    // w = max mist-top Y (bounds the raymarch's slab clip).
    vec4 uMistMapInfo;
    // x = erode with the baked NoiseVolume (0 = analytic fbm3 - the A/B
    // and the no-caps fallback), y = march steps, z = erosion detail
    // dropout distance (m; beyond it the march skips the noise taps),
    // w = sun-beam gain (the normalized phase alone is too dim against
    // the full-sky ambient).
    vec4 uMistDetailInfo;
    // Mist light shaping (the silver-lining kit): x = forward HG lobe g
    // (higher = tighter, brighter rim toward the sun), y = backscatter
    // lobe weight, z = ambient gain (lower darkens the mist body so the
    // sun beam pops), w = ambient floor in shadow.
    vec4 uMistLightInfo;
    // Fog altitude envelope: x = ceiling falloff (1/m above sea level,
    // weather-crossfaded) - the fog is a GROUND layer, so upward rays exit
    // it and the sky stays readable while the horizon keeps its fog and
    // ray curtains. y = horizon-closure distance (m; the far-terrain reach
    // or the streaming ring, 0 = off), z = the streaming ring itself (m -
    // the far mesh's sink bias), w = free.
    vec4 uFogLayerInfo;
    // Volumetric sky clouds (skyclouds.frag): x = active (also gates the
    // 2D dome layer off), y = slab thickness (m), z = extinction sigma
    // (1/m), w = erosion strength. The slab BASE is the weather's cloud
    // height (uCloudInfo.y); coverage/scale stay the shared uCloudInfo
    // lanes so the baked ground shadows match. Resolved only: the
    // reflection pass keeps the 2D dome layer.
    vec4 uCloudVolInfo;
    // x = body sun gain (multi-octave scattering - the soft luminous
    // core), y = body HG lobe g, z = ambient gain, w = LINING gain (the
    // direct-transmission term exp(-tau)*HG - the silver lining).
    vec4 uCloudVolLightInfo;
    // x = thickness<->coverage spread (at 4 a full sky is 5x thick),
    // y = lining HG lobe g (tighter = the halo hugs the sun), z = powder
    // strength (dark-edge term away from the sun), w = puffiness (the
    // fractal edge-erosion pass - cauliflower florets).
    vec4 uCloudVolShapeInfo;
    // x = rim gain (silhouette glow where the cloud is thin along the
    // VIEW ray - the sun-path lining cannot produce it), y = rim HG lobe
    // g, z = storm darkening 0..10, EXPONENTIAL (exp(-storm x coverage):
    // ambient fully, sun body at 85%, bases fastest; lining and rim keep
    // their fire), w unused.
    vec4 uCloudVolRimInfo;
    // x = mist puffiness (fractal edge florets on the patch borders - the
    // cloud recipe ported), yzw unused.
    vec4 uMistPuffInfo;
    // x = water debug view mode (0 off; the render panel's Water combo),
    // yzw unused.
    vec4 uWaterDebugInfo;
    // Seasons: x = autumn blend 0..1, y = leaf fall 0..1, zw unused.
    // Applied per leaf-mask atlas slot through uLeafSeason.
    vec4 uSeasonInfo;
    // Per leaf-mask atlas slot: rgb = autumn tint, a = seasonality
    // (0 = evergreen - conifers keep needles and color).
    vec4 uLeafSeason[8];
    // Splat material detail (docs/PAYSAGE.md sec. 1.6): x = height-blend band
    // depth (0 = plain weighted blend), y = macro tint strength (0 = off),
    // z = detail-normal fade end (m), w = parallax occlusion reach
    // (m, 0 = off).
    vec4 uSplatDetailInfo;
    // Region shading maps (TerrainShadeMap): xy = center, z = 1/span,
    // w = valid.
    vec4 uTerrainShadeMapInfo;
    // Splat anti-repetition + POM: x = bi-frequency variety strength
    // (0 = off - the second, non-harmonic tap that keeps the 4 m tile grid
    // from ever repeating exactly), y = POM self-shadow strength, z = POM
    // relief depth (uv units), w = free.
    vec4 uSplatVarietyInfo;
    // Grass species table (GrassSpecies.hpp is the source, the composer
    // copies it; kGrassSpeciesCount rows) - grass.vert indexes by the
    // instance's species lane. Shape: x = height (applied at scatter,
    // unused in shader), y = width, z = lean, w = tip profile.
    vec4 uGrassSpeciesShape[6];
    // rgb = base tint x inherited ground albedo, w unused.
    vec4 uGrassSpeciesBase[6];
    // rgb = tip tint x inherited ground albedo, w unused.
    vec4 uGrassSpeciesTip[6];
    // SSAO (ssao.frag): x = strength, y = world radius (m); z = screen-
    // space displacement amplitude (m, 0 = off - ssdm_common.glsl);
    // w = far-cascade viewport scale (shadow.glsl samples the lower-left
    // quarter of far layers - ShadowMapper contract).
    vec4 uSsaoInfo;
    // Terrain surface response: x = per-material AO strength (cooked
    // ORM), y = wetness darkening, z = wet sheen, w = snow sheen. The
    // sheen is the only specular the stylized terrain has - it lives
    // exclusively on wet/snowy ground, dry land stays pure diffuse.
    vec4 uSurfSheenInfo;
    // Real-time water sim window (WaterSystem): xy = window ORIGIN (min
    // corner, world m), z = 1/span, w = valid (0/1).
    vec4 uWaterSimMapInfo;
    // x = trusted inset (m), y = edge fade band (m), z = debug mode
    // (0 normal, 1 force baked, 2 seam overlay), w unused.
    vec4 uWaterSimTuneInfo;
    // Frozen sim windows (WaterSystem::simFrozen - static meshes of past
    // window states, drawn beyond the live rect): per entry xy = window
    // ORIGIN (min corner), z = 1/span (0 = slot empty), w unused. Ordered
    // oldest -> NEWEST (the shader resolves overlaps by recency).
    vec4 uWaterSimFrozen[4];
    // Fire scorch mask (FireScorchMap::info, firescorch.glsl): xy = window
    // origin, z = 1/texel, w = cells per side (0 = nothing burnt).
    vec4 uFireScorchInfo;
    // The burnt ground's look (FireLook): x = bed intensity (0 = off),
    // y = pulse, z = erode, w = noise volume ready.
    vec4 uFireEmberInfo;
    // rgb = hot ember colour, w = coal scale.
    vec4 uFireEmberHot;
    // rgb = cold ember colour, w = ash.
    vec4 uFireEmberCold;
    // rgb = charred colour, w = grass cull threshold.
    vec4 uFireCharInfo;
    // The flames' flipbook (RenderSnapshot::FlameSheet): x = columns,
    // y = rows, z = fps, w = frame aspect.
    vec4 uFireFlameInfo;
    // x = HDR boost, y = posterize bands (0 = off), zw unused.
    vec4 uFireFlameLook;
    // The wind's DIRECTION: xy = unit XZ it blows toward (the weather's
    // windDirectionDeg); zw = its accumulated drift (metres at strength
    // 1) - what the clouds and the mist ride, so a turning wind bends
    // their path instead of jumping it. Strength and clock stay in
    // uWindInfo.
    vec4 uWindDirInfo;
    // Fog optical-tail compression (applyFog): x = knee distance (m -
    // kept above the froxel reach cap so the volumetric band and the
    // weather TOMLs never see it), y = tail factor (fraction of each
    // meter past the knee that still counts as optical depth; 1 =
    // legacy), z = sea-quad half-span (m - the water sheet follows the
    // farthest terrain ring; kSeaQuadHalfSpan is its floor), w unused.
    vec4 uFogTailInfo;
    // Scene-wide flags the vegetation and interior shading read. x = tree
    // bark textures resident (tree.frag gate), y = interior flag (mesh/
    // skinned/locallights switch to the hemispheric ambient + wrap/bounce
    // and tonemap.frag to the hue-preserving curve indoors; 0 keeps the
    // exterior byte-identical), z = real-tree fade-out distance (m) - the
    // far impostors fade IN against it (far_tree.vert), w = free. In BASE:
    // the reflection pass reads the same flags.
    vec4 uVegInfo;
};

#endif
