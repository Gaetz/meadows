#pragma once

#include <span>

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

// The ONE description of the per-frame UBO (binding 0) and of the scalar
// constants the C++ side shares with GLSL. Two X-macro tables generate
// everything that used to be written by hand on both sides:
//
//   MEADOWS_FRAME_LANES(X)      -> struct FrameUniforms (FrameUniforms.hpp),
//                                  the FrameUbo block (shaders/frame_ubo.glsl),
//                                  the lane descriptors below (frameLanes()).
//   MEADOWS_FRAME_CONSTANTS(X)  -> the `const` preamble of frame_ubo.glsl.
//
// frame_ubo.glsl is a checked-in file (shader authors and the offline glslc
// check keep a real file); frameUboGlsl() regenerates its text and
// tests/FrameLanesTest.cpp pins the file to it.
//
// The workflow is: EDIT THE TABLE, the struct and the shaders follow.
// Add, remove, rename or reorder a lane here, rebuild, run
// `meadows-tests -tc="frame lanes*"` and paste the printed text into
// frame_ubo.glsl; then update the offset list in FrameUniforms.hpp (the
// test prints the shifted offsets too). Because both sides come from this
// one table, a mid-table edit cannot desync them - the only things to
// keep by hand are the C++ writers (FrameComposer) and the shader READS
// of a renamed or removed channel (grep the glslName). A lane nothing
// reads is deleted, not kept zeroed.
//
// Layout rules (docs/RENDERING.md §2.3): std140, every lane vec4- or
// mat4-sized, so the C++ offsets equal the GLSL ones with no padding.
//
// Lane columns:
//   cppType   C++ member type (an alias below for arrays — the X-macro needs
//             one comma-free token)
//   cppName   C++ member name (FrameComposer writes it)
//   init      default member initializer, parenthesised: () or (x, y, z, w)
//   glslType  GLSL element type (mat4 / vec4)
//   glslName  GLSL identifier: `u` + cppName with its first letter upper-cased
//   count     array length, 0 for a scalar lane
//   doc       channel meanings; "\n" separates comment lines (<= 72 cols).
//             A channel no shader reads is documented "free" and written 0.
//
// Constant columns: (cppType i32|f32, glslName, C++ constant expression, doc).
// The expression names the OWNING declaration (LightClusters::kClusterX…),
// so the table is expanded only where those headers are included
// (FrameLanes.cpp).

namespace render {

using FrameMat4x3 = array<Mat4, 3>;
using FrameMat4x4 = array<Mat4, 4>;
using FrameVec4x4 = Vec4[4];
using FrameVec4x6 = Vec4[6];
using FrameVec4x8 = Vec4[8];

// clang-format off
#define MEADOWS_FRAME_LANES(X) \
    X(Mat4, viewProj, (), mat4, uViewProj, 0, \
      "World -> clip for this view.") \
    X(Mat4, invViewProj, (), mat4, uInvViewProj, 0, \
      "NDC -> world, for fullscreen ray reconstruction.") \
    X(Vec4, cameraPos, (), vec4, uCameraPos, 0, \
      "xyz = camera world position, w = 1.") \
    X(Vec4, time, (), vec4, uTime, 0, \
      "x = seconds since scene start, y = free,\n" \
      "z = volumetric shaft intensity (0 indoors unless the froxel fog\n" \
      "runs), w = debug buffer view index (tonemap.frag).") \
    X(Vec4, sunDirection, (), vec4, uSunDirection, 0, \
      "xyz = normalized, points TOWARD the sun; w = frame dt (s) for the\n" \
      "auto-exposure adaptation (adapt.frag).") \
    X(Vec4, sunColor, (), vec4, uSunColor, 0, \
      "rgb = sun light color (black indoors), w = sun disc intensity.") \
    X(Vec4, sunGlowColor, (), vec4, uSunGlowColor, 0, \
      "rgb = sky halo/afterglow (outlives the disc; black indoors),\n" \
      "w = grade vibrance (0 = neutral).") \
    X(Vec4, ambientColor, (), vec4, uAmbientColor, 0, \
      "rgb = sky ambient term (indoors: the room ambient, daylight-\n" \
      "coupled), w = stylized-lighting blend (0 classic wrap, 1 BotW step\n" \
      "ramp - stylized.glsl).") \
    X(Vec4, zenithColor, (), vec4, uZenithColor, 0, \
      "rgb = sky gradient top, w = grade split-tone (0 = neutral).") \
    X(Vec4, horizonColor, (), vec4, uHorizonColor, 0, \
      "rgb = horizon on the SUN side (warm at dusk), w = grade contrast\n" \
      "(1 = neutral).") \
    X(Vec4, horizonFarColor, (), vec4, uHorizonFarColor, 0, \
      "rgb = horizon opposite the sun (already night), w = auto-exposure\n" \
      "minimum.") \
    X(Vec4, terrainInfo, (), vec4, uTerrainInfo, 0, \
      "x = sea level, y = snow line (m), z = splat UV scale (tiles/meter),\n" \
      "w = planar reflections active (0/1).") \
    X(Vec4, postInfo, (), vec4, uPostInfo, 0, \
      "x = filmic tonemap (0/1), y = exposure, z = cascade debug view\n" \
      "(0/1), w = bloom intensity.") \
    X(Vec4, fogInfo, (), vec4, uFogInfo, 0, \
      "x = density, y = height falloff (1/m), z = low-altitude boost,\n" \
      "w = start distance (m). Indoors: {0, 0.02, 0, 1e5} (no fog).") \
    X(FrameMat4x3, sunViewProj, (), mat4, uSunViewProj, 3, \
      "World -> cascade shadow clip space, one per cascade.") \
    X(Vec4, cascadeSplits, (), vec4, uCascadeSplits, 0, \
      "xyz = cascade far view-distances (m), w = free.") \
    X(Vec4, shadowInfo, (), vec4, uShadowInfo, 0, \
      "xyz = world texel size per cascade, w = shadow strength\n" \
      "(0 = shadows off).") \
    X(Vec4, screenInfo, (), vec4, uScreenInfo, 0, \
      "xy = viewport size (px), zw = 1/size.") \
    X(Vec4, cloudInfo, (), vec4, uCloudInfo, 0, \
      "x = coverage [0,1], y = layer height (m), z = pattern scale (1/m),\n" \
      "w = shadow strength.") \
    X(Vec4, sunScreen, (), vec4, uSunScreen, 0, \
      "xy = sun position in screen UV, z = shaft visibility fade,\n" \
      "w = god-ray intensity (all neutral indoors).") \
    X(Vec4, cloudMapInfo, (), vec4, uCloudMapInfo, 0, \
      "xy = baked cloud-field center (world XZ, snapped to the map cell),\n" \
      "z = 1/span, w = auto-exposure maximum.") \
    X(Vec4, waterMapInfo, (), vec4, uWaterMapInfo, 0, \
      "xy = pool-depth map center (world XZ), z = 1/span, w unused.") \
    X(Vec4, windInfo, (), vec4, uWindInfo, 0, \
      "x = accumulated wind time (s, speed-scaled - keeps cloud/wave phase\n" \
      "continuous when the weather changes the wind), y = sway strength,\n" \
      "z = water chop multiplier, w = auto-exposure active (0/1).") \
    X(Vec4, terrainLightInfo, (), vec4, uTerrainLightInfo, 0, \
      "Terrain light map (far terrain shadows + sky aperture): xy = map\n" \
      "center (world XZ), z = 1/span, w = strength (0 = feature off: no\n" \
      "bake yet, toggled off, or indoors).") \
    X(Vec4, submersionInfo, (-1.0e6f, 0.0f, 0.0f, 0.0f), vec4, \
      uSubmersionInfo, 0, \
      "x = the EFFECTIVE water surface Y above the camera (sea level\n" \
      "outdoors, a volume's top when inside one, -1e6 = dry) - the tonemap\n" \
      "submersion reads this, not the sea level. yzw unused.") \
    X(Vec4, stormInfo, (), vec4, uStormInfo, 0, \
      "x = storm front 0-1 (horizon cumulonimbus), y = rain intensity 0-1\n" \
      "(streaks + wetness + the occlusion matrix; 0 indoors), zw unused.") \
    X(Mat4, rainOcclusionViewProj, (), mat4, uRainOcclusionViewProj, 0, \
      "World -> top-down ortho clip for the rain occlusion depth (no rain\n" \
      "under roofs; 40 m around the camera). Zero when it does not rain.") \
    X(Vec4, grassBendInfo, (), vec4, uGrassBendInfo, 0, \
      "Interactive grass bending: xy = the player's feet XZ, z = feet Y,\n" \
      "w = bend radius (0 = off, e.g. Fly mode).") \
    X(Vec4, grassShapeInfo, (0.95f, 0.045f, 12.5f, 25.0f), vec4, \
      uGrassShapeInfo, 0, \
      "Meadow tuning, live from the render panel's \"Grass\" category\n" \
      "(GrassRenderTuning): x = blade height (m), y = half width (m),\n" \
      "zw = high-detail near/far (m).") \
    X(Vec4, grassLodInfo, (10.0f, 70.0f, 0.20f, 1.7f), vec4, uGrassLodInfo, \
      0, \
      "xy = density thinning start/end (m), z = far density floor,\n" \
      "w = width compensation at far density (wider blades, same mass).") \
    X(Vec4, grassBaseTint, (1.0f, 1.0f, 1.0f, 140.0f), vec4, uGrassBaseTint, \
      0, \
      "rgb = base tint x root ground albedo, w = distance fade start (m).") \
    X(Vec4, grassTipTint, (1.0f, 1.0f, 1.0f, 190.0f), vec4, uGrassTipTint, \
      0, \
      "rgb = tip tint x root ground albedo, w = distance fade end (m).") \
    X(Vec4, giInfo, (), vec4, uGiInfo, 0, \
      "The GiTechnique switch (gi.glsl): x = technique (0 classic,\n" \
      "1 radiance cascades - surface shaders swap their ambient term for\n" \
      "the merged cascade-0 sample), y = intensity, z = edge fade width\n" \
      "(m), w = grid resolution. Resolved only: the reflection pass stays\n" \
      "classic, byte-identical exterior.") \
    X(Vec4, giGridInfo, (), vec4, uGiGridInfo, 0, \
      "xyz = cascade-0 grid origin, w = probe spacing.") \
    X(Vec4, giBandInfo, (0.0f, 0.3f, 0.7f, 0.0f), vec4, uGiBandInfo, 0, \
      "GI ramp: x = band count between floor and classic ambient\n" \
      "(0 = smooth), y = anti-aliasing width across a band edge, z = GI\n" \
      "ambient floor (fraction of classic - grid-border seam killer),\n" \
      "w unused.") \
    X(Vec4, leafLodInfo, (4.0f, 7.0f, 0.0f, 0.0f), vec4, uLeafLodInfo, 0, \
      "Foliage-card leaf cutout -> solid ramp (tree.frag/shadow_prop.frag):\n" \
      "xy = mip window start/end of the blend. z = mirror-pass flag (the\n" \
      "planar reflection sets 1): billboard cards re-aim at the mirrored\n" \
      "camera, so their winding does NOT flip with the pass's inverted\n" \
      "front face - tree.vert flips the card corners instead. w unused.") \
    X(Vec4, fogSunInfo, (0.5f, 8.0f, 0.0f, 0.0f), vec4, uFogSunInfo, 0, \
      "Fog sun single-scatter: x = strength (weather-crossfaded), y = phase\n" \
      "exponent, z = volumetric march reach (m) - the analytic fog of the\n" \
      "surfaces starts where the march ends (froxel reach, or 1400 for the\n" \
      "2D march; 0 = no march, full analytic fog: reflection pass, night),\n" \
      "w = interior dust density (froxel fog indoors).") \
    X(Vec4, stylizedDiffuseInfo, (0.02f, 0.09f, 0.32f, 0.40f), vec4, \
      uStylizedDiffuseInfo, 0, \
      "Stylized diffuse ramp (stylized.glsl): xy = terminator smoothstep\n" \
      "edges, zw = full-light smoothstep edges.") \
    X(Vec4, stylizedShadowInfo, (0.45f, 0.55f, 0.0f, 0.6f), vec4, \
      uStylizedShadowInfo, 0, \
      "Stylized shadow snap: xy = window edges, z = shadow floor,\n" \
      "w = half-tone level of the diffuse ramp.") \
    X(Vec4, clusterInfo, (0.0f, 800.0f, 0.0f, 0.0f), vec4, uClusterInfo, 0, \
      "Clustered forward (docs/RENDERING.md sec. 5): x = clustered path active\n" \
      "(0 = legacy nearest-lights loop), y = cluster grid far reach (m) -\n" \
      "the froxel slicing's far, so both grids share their z slices.\n" \
      "zw unused. Resolved only: the reflection pass keeps the legacy loop.") \
    X(FrameMat4x4, keyShadowAtlasViewProj, (), mat4, uKeyShadowAtlasViewProj, \
      4, \
      "Key-shadow ATLAS (docs/RENDERING.md sec. 5): world -> tile clip per\n" \
      "slot - up to 4 shadowed lights, one 1024^2 tile each in the 2048^2\n" \
      "key-shadow target. A light finds its slot through LightsUbo\n" \
      "windowInfo.z (slot+1, 0 = unshadowed). Resolved only.") \
    X(Vec4, grassShadeInfo, (1.0f, 0.5f, 0.0f, 0.0f), vec4, uGrassShadeInfo, \
      0, \
      "Grass shading knobs (grass.vert/.frag): x = root-AO floor\n" \
      "(1 = none), y = tip sheen strength, z = near blade-normal share\n" \
      "(0 = blades light like the ground), w unused.") \
    X(Vec4, grassBladeInfo, (1.0f, 1.0f, 0.0f, 0.0f), vec4, uGrassBladeInfo, \
      0, \
      "xy = whole-blade brightness hash range (min/max), z = blade middle\n" \
      "darkening, w = backscatter strength.") \
    X(Vec4, stylizedSpecInfo, (0.35f, 0.35f, 24.0f, 0.0f), vec4, \
      uStylizedSpecInfo, 0, \
      "Stylized specular band on characters/props (mesh/skinned.frag):\n" \
      "x = strength, y = band threshold (lower = wider highlight),\n" \
      "z = Blinn-Phong exponent, w unused.") \
    X(Vec4, mistInfo, (0.0f, 1200.0f, 0.5f, 0.6f), vec4, uMistInfo, 0, \
      "Ground mist (mist.frag, docs/RENDERING.md sec. 3.5): x = extinction\n" \
      "sigma (1/m, weather-crossfaded; 0 = feature off - THE shader gate),\n" \
      "y = reach (m), z = coverage threshold, w = coverage softness.\n" \
      "Resolved only, exterior only.") \
    X(Vec4, mistShapeInfo, (0.0035f, 0.02f, 0.2f, 49.0f), vec4, \
      uMistShapeInfo, 0, \
      "x = coverage pattern scale (1/m), y = erosion noise scale (1/m),\n" \
      "z = erosion strength, w = lift (m, raises the baked mist top).") \
    X(Vec4, mistMapInfo, (), vec4, uMistMapInfo, 0, \
      "Mist valley map (MistMap): xy = bake center (world XZ), z = 1/span,\n" \
      "w = max mist-top Y (bounds the raymarch's slab clip).") \
    X(Vec4, mistDetailInfo, (0.0f, 16.0f, 400.0f, 10.0f), vec4, \
      uMistDetailInfo, 0, \
      "x = erode with the baked NoiseVolume (0 = analytic fbm3 - the A/B\n" \
      "and the no-caps fallback), y = march steps, z = erosion detail\n" \
      "dropout distance (m; beyond it the march skips the noise taps),\n" \
      "w = sun-beam gain (the normalized phase alone is too dim against\n" \
      "the full-sky ambient).") \
    X(Vec4, mistLightInfo, (0.95f, 0.8f, 1.25f, 0.6f), vec4, uMistLightInfo, \
      0, \
      "Mist light shaping (the silver-lining kit): x = forward HG lobe g\n" \
      "(higher = tighter, brighter rim toward the sun), y = backscatter\n" \
      "lobe weight, z = ambient gain (lower darkens the mist body so the\n" \
      "sun beam pops), w = ambient floor in shadow.") \
    X(Vec4, fogLayerInfo, (0.0035f, 0.0f, 0.0f, 0.0f), vec4, uFogLayerInfo, \
      0, \
      "Fog altitude envelope: x = ceiling falloff (1/m above sea level,\n" \
      "weather-crossfaded) - the fog is a GROUND layer, so upward rays exit\n" \
      "it and the sky stays readable while the horizon keeps its fog and\n" \
      "ray curtains. y = horizon-closure distance (m; the far-terrain reach\n" \
      "or the streaming ring, 0 = off), z = the streaming ring itself (m -\n" \
      "the far mesh's sink bias), w = free.") \
    X(Vec4, cloudVolInfo, (0.0f, 440.0f, 0.065f, 0.31f), vec4, uCloudVolInfo, \
      0, \
      "Volumetric sky clouds (skyclouds.frag): x = active (also gates the\n" \
      "2D dome layer off), y = slab thickness (m), z = extinction sigma\n" \
      "(1/m), w = erosion strength. The slab BASE is the weather's cloud\n" \
      "height (uCloudInfo.y); coverage/scale stay the shared uCloudInfo\n" \
      "lanes so the baked ground shadows match. Resolved only: the\n" \
      "reflection pass keeps the 2D dome layer.") \
    X(Vec4, cloudVolLightInfo, (19.9f, 0.3f, 0.9f, 30.2f), vec4, \
      uCloudVolLightInfo, 0, \
      "x = body sun gain (multi-octave scattering - the soft luminous\n" \
      "core), y = body HG lobe g, z = ambient gain, w = LINING gain (the\n" \
      "direct-transmission term exp(-tau)*HG - the silver lining).") \
    X(Vec4, cloudVolShapeInfo, (3.4f, 0.8f, 1.0f, 0.5f), vec4, \
      uCloudVolShapeInfo, 0, \
      "x = thickness<->coverage spread (at 4 a full sky is 5x thick),\n" \
      "y = lining HG lobe g (tighter = the halo hugs the sun), z = powder\n" \
      "strength (dark-edge term away from the sun), w = puffiness (the\n" \
      "fractal edge-erosion pass - cauliflower florets).") \
    X(Vec4, cloudVolRimInfo, (25.0f, 0.75f, 7.4f, 0.0f), vec4, \
      uCloudVolRimInfo, 0, \
      "x = rim gain (silhouette glow where the cloud is thin along the\n" \
      "VIEW ray - the sun-path lining cannot produce it), y = rim HG lobe\n" \
      "g, z = storm darkening 0..10, EXPONENTIAL (exp(-storm x coverage):\n" \
      "ambient fully, sun body at 85%, bases fastest; lining and rim keep\n" \
      "their fire), w unused.") \
    X(Vec4, mistPuffInfo, (0.5f, 0.0f, 0.0f, 0.0f), vec4, uMistPuffInfo, 0, \
      "x = mist puffiness (fractal edge florets on the patch borders - the\n" \
      "cloud recipe ported), yzw unused.") \
    X(Vec4, waterDebugInfo, (0.0f, 0.0f, 0.0f, 0.0f), vec4, uWaterDebugInfo, \
      0, \
      "x = water debug view mode (0 off; the render panel's Water combo),\n" \
      "yzw unused.") \
    X(Vec4, seasonInfo, (0.0f, 0.0f, 0.0f, 0.0f), vec4, uSeasonInfo, 0, \
      "Seasons: x = autumn blend 0..1, y = leaf fall 0..1, zw unused.\n" \
      "Applied per leaf-mask atlas slot through uLeafSeason.") \
    X(FrameVec4x8, leafSeason, (), vec4, uLeafSeason, 8, \
      "Per leaf-mask atlas slot: rgb = autumn tint, a = seasonality\n" \
      "(0 = evergreen - conifers keep needles and color).") \
    X(Vec4, splatDetailInfo, (0.15f, 0.0f, 0.0f, 0.0f), vec4, \
      uSplatDetailInfo, 0, \
      "Splat material detail (docs/PAYSAGE.md sec. 1.6): x = height-blend band\n" \
      "depth (0 = plain weighted blend), y = macro tint strength (0 = off),\n" \
      "z = detail-normal fade end (m), w = parallax occlusion reach\n" \
      "(m, 0 = off).") \
    X(Vec4, terrainShadeMapInfo, (0.0f, 0.0f, 0.0f, 0.0f), vec4, \
      uTerrainShadeMapInfo, 0, \
      "Region shading maps (TerrainShadeMap): xy = center, z = 1/span,\n" \
      "w = valid.") \
    X(Vec4, splatVarietyInfo, (0.5f, 0.0f, 0.03f, 0.0f), vec4, \
      uSplatVarietyInfo, 0, \
      "Splat anti-repetition + POM: x = bi-frequency variety strength\n" \
      "(0 = off - the second, non-harmonic tap that keeps the 4 m tile grid\n" \
      "from ever repeating exactly), y = POM self-shadow strength, z = POM\n" \
      "relief depth (uv units), w = free.") \
    X(FrameVec4x6, grassSpeciesShape, (), vec4, uGrassSpeciesShape, 6, \
      "Grass species table (GrassSpecies.hpp is the source, the composer\n" \
      "copies it; kGrassSpeciesCount rows) - grass.vert indexes by the\n" \
      "instance's species lane. Shape: x = height (applied at scatter,\n" \
      "unused in shader), y = width, z = lean, w = tip profile.") \
    X(FrameVec4x6, grassSpeciesBase, (), vec4, uGrassSpeciesBase, 6, \
      "rgb = base tint x inherited ground albedo, w unused.") \
    X(FrameVec4x6, grassSpeciesTip, (), vec4, uGrassSpeciesTip, 6, \
      "rgb = tip tint x inherited ground albedo, w unused.") \
    X(Vec4, ssaoInfo, (0.85f, 0.7f, 0.0f, 0.0f), vec4, uSsaoInfo, 0, \
      "SSAO (ssao.frag): x = strength, y = world radius (m); z = screen-\n" \
      "space displacement amplitude (m, 0 = off - ssdm_common.glsl);\n" \
      "w = far-cascade viewport scale (shadow.glsl samples the lower-left\n" \
      "quarter of far layers - ShadowMapper contract).") \
    X(Vec4, surfSheenInfo, (0.7f, 0.6f, 0.5f, 0.04f), vec4, uSurfSheenInfo, \
      0, \
      "Terrain surface response: x = per-material AO strength (cooked\n" \
      "ORM), y = wetness darkening, z = wet sheen, w = snow sheen. The\n" \
      "sheen is the only specular the stylized terrain has - it lives\n" \
      "exclusively on wet/snowy ground, dry land stays pure diffuse.") \
    X(Vec4, waterSimMapInfo, (0.0f, 0.0f, 0.0f, 0.0f), vec4, \
      uWaterSimMapInfo, 0, \
      "Real-time water sim window (WaterSystem): xy = window ORIGIN (min\n" \
      "corner, world m), z = 1/span, w = valid (0/1).") \
    X(Vec4, waterSimTuneInfo, (64.0f, 32.0f, 0.0f, 0.0f), vec4, \
      uWaterSimTuneInfo, 0, \
      "x = trusted inset (m), y = edge fade band (m), z = debug mode\n" \
      "(0 normal, 1 force baked, 2 seam overlay), w unused.") \
    X(FrameVec4x4, waterSimFrozen, (), vec4, uWaterSimFrozen, 4, \
      "Frozen sim windows (WaterSystem::simFrozen - static meshes of past\n" \
      "window states, drawn beyond the live rect): per entry xy = window\n" \
      "ORIGIN (min corner), z = 1/span (0 = slot empty), w unused. Ordered\n" \
      "oldest -> NEWEST (the shader resolves overlaps by recency).") \
    X(Vec4, fireScorchInfo, (), vec4, uFireScorchInfo, 0, \
      "Fire scorch mask (FireScorchMap::info, firescorch.glsl): xy = window\n" \
      "origin, z = 1/texel, w = cells per side (0 = nothing burnt).") \
    X(Vec4, fireEmberInfo, (), vec4, uFireEmberInfo, 0, \
      "The burnt ground's look (FireLook): x = bed intensity (0 = off),\n" \
      "y = pulse, z = erode, w = noise volume ready.") \
    X(Vec4, fireEmberHot, (), vec4, uFireEmberHot, 0, \
      "rgb = hot ember colour, w = coal scale.") \
    X(Vec4, fireEmberCold, (), vec4, uFireEmberCold, 0, \
      "rgb = cold ember colour, w = ash.") \
    X(Vec4, fireCharInfo, (), vec4, uFireCharInfo, 0, \
      "rgb = charred colour, w = grass cull threshold.") \
    X(Vec4, fireFlameInfo, (), vec4, uFireFlameInfo, 0, \
      "The flames' flipbook (RenderSnapshot::FlameSheet): x = columns,\n" \
      "y = rows, z = fps, w = frame aspect.") \
    X(Vec4, fireFlameLook, (), vec4, uFireFlameLook, 0, \
      "x = HDR boost, y = posterize bands (0 = off), zw unused.") \
    X(Vec4, windDirInfo, (1.0f, 0.0f, 0.0f, 0.0f), vec4, uWindDirInfo, 0, \
      "The wind's DIRECTION: xy = unit XZ it blows toward (the weather's\n" \
      "windDirectionDeg); zw = its accumulated drift (metres at strength\n" \
      "1) - what the clouds and the mist ride, so a turning wind bends\n" \
      "their path instead of jumping it. Strength and clock stay in\n" \
      "uWindInfo.") \
    X(Vec4, fogTailInfo, (2500.0f, 0.30f, 9000.0f, 0.0f), vec4, uFogTailInfo, \
      0, \
      "Fog optical-tail compression (applyFog): x = knee distance (m -\n" \
      "kept above the froxel reach cap so the volumetric band and the\n" \
      "weather TOMLs never see it), y = tail factor (fraction of each\n" \
      "meter past the knee that still counts as optical depth; 1 =\n" \
      "legacy), z = sea-quad half-span (m - the water sheet follows the\n" \
      "farthest terrain ring; kSeaQuadHalfSpan is its floor), w unused.") \
    X(Vec4, vegInfo, (), vec4, uVegInfo, 0, \
      "Scene-wide flags the vegetation and interior shading read. x = tree\n" \
      "bark textures resident (tree.frag gate), y = interior flag (mesh/\n" \
      "skinned/locallights switch to the hemispheric ambient + wrap/bounce\n" \
      "and tonemap.frag to the hue-preserving curve indoors; 0 keeps the\n" \
      "exterior byte-identical), z = real-tree fade-out distance (m) - the\n" \
      "far impostors fade IN against it (far_tree.vert), w = free. In BASE:\n" \
      "the reflection pass reads the same flags.")

// Scalars both sides must agree on. The GLSL side reads them from the
// generated preamble of frame_ubo.glsl; the C++ side keeps its declaration
// where the owning system lives (the expression column points at it).
#define MEADOWS_FRAME_CONSTANTS(X) \
    X(i32, kClusterX, render::LightClusters::kClusterX, \
      "Light-cluster grid cells along X (LightClusters, clusters.glsl).") \
    X(i32, kClusterY, render::LightClusters::kClusterY, \
      "Light-cluster grid cells along Y.") \
    X(i32, kClusterZ, render::LightClusters::kClusterZ, \
      "Light-cluster depth slices - shared with the froxel-fog grid.") \
    X(i32, kClusterSlots, render::LightClusters::kSlots, \
      "Per-cluster list capacity: slot 0 = light count, the rest indices.") \
    X(i32, kMaxLights, render::kMaxLights, \
      "LightsUbo array capacity (FrameDecisions.hpp LightsUniforms).") \
    X(i32, kFallbackLights, render::kFallbackLights, \
      "Legacy per-pixel light loop cap when the clustered path is off.") \
    X(i32, kRcMaxLights, render::RadianceCascades::kMaxLights, \
      "RcUbo light array capacity - the GI takes the nearest lights.") \
    X(i32, kMaxWaterMaterials, render::WaterSystem::kMaxWaterMaterials, \
      "WaterMaterialsUbo table size (water_surface.glsl).") \
    X(i32, kGrassSpeciesCount, render::kGrassSpeciesCount, \
      "Rows of the grass species tables (GrassSpecies.hpp).") \
    X(f32, kMistTopOffset, render::MistMap::kTopOffset, \
      "MistMap R quantization: mist top = seaLevel + offset + r * range.") \
    X(f32, kMistTopRange, render::MistMap::kTopRange, \
      "MistMap R quantization range (m).") \
    X(f32, kFarTreeFar, render::FarTerrain::Config {}.treeFar, \
      "Inner far ring's impostor reach (m) - far_tree.vert fades out over\n" \
      "the last 1000 m; the bake stops scattering impostors there.") \
    X(f32, kSeaQuadHalfSpan, render::FarTerrain::Config {}.span * 0.5f, \
      "Sea sheet half-span floor (m): half the inner far ring's span.")
// clang-format on

// One lane of the generated UBO, as the tests and tools see it.
struct FrameLaneInfo {
    const char* cppName;
    const char* glslType;
    const char* glslName;
    u32 count;  // array length, 0 = scalar
    u32 offset; // byte offset in FrameUniforms (== the std140 offset)
    u32 size;   // sizeof the C++ member
    const char* doc;
};

// The lanes of MEADOWS_FRAME_LANES, in declaration order.
std::span<const FrameLaneInfo> frameLanes();

// The full text of shaders/frame_ubo.glsl: the constants preamble and the
// FrameUbo block, generated from the two tables. The checked-in file must
// equal this byte for byte (tests/FrameLanesTest.cpp).
str frameUboGlsl();

} // namespace render
