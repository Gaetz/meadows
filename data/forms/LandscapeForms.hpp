#pragma once

#include "data/forms/Form.hpp"

// Landscape & weather tuning Forms. They live in this lib (not
// game/scenes) so TOOLS see them — the cooker and
// the Game DB editor register every family, and an exe-local Form type is
// invisible to both.
//
// LandscapeTuningForm: every startup value of the 3D landscape renderer
// (§5 precedent: StatsTuningForm). One record, canonical guid, resolved by
// resolveLandscapeTuning(); the scene copies it into plain params.
// WeatherForm: one full weather state, crossfaded by the scene (~30 s).

namespace data {

class FormTypeRegistry;
class FormDatabase;

struct LandscapeTuningForm : Form {
    // Terrain shape (render::TerrainParams).
    u32 terrainSeed { 1337 };
    f32 hillWavelength { 500.0f };
    f32 hillAmplitude { 75.0f };
    f32 mountainWavelength { 2000.0f };
    f32 mountainAmplitude { 270.0f };
    f32 seaLevel { 21.0f }; // keep equal to render::kDefaultSeaLevel
                            // (engine/terrain/TerrainBase.hpp)
    // Streaming ring radius in 64 m chunks — the draw distance
    // ("voir des paysages"); applyFog's horizon closure tracks it.
    // Perf-sensitive: chunks grow as (2r+1)^2.
    i32 terrainViewRadius { 30 };
    // Distant silhouettes past the ring (FarTerrain, ~12 km coarse mesh).
    bool farTerrain { true };
    // Terrain materials.
    f32 snowLine { 165.0f };     // meters
    f32 splatUvScale { 0.25f };  // tiles per meter
    // Height-blend band depth between splat layers (0 = plain weighted
    // blend; ~0.1-0.2 = crisp material interfaces). docs/TERRAIN-TEXTURING.md.
    f32 splatBlendDepth { 0.15f };
    // Macro-tint strength (0 = off; above ~0.4 the tint crushes the
    // materials' own variation — brief guardrail).
    f32 terrainTintStrength { 0.3f };
    // Near-field detail-normal fade end (meters; 0 = detail off). An
    // unfaded detail normal aliases in the background.
    f32 splatDetailFade { 24.0f };
    // Parallax occlusion mapping reach (meters; 0 = off). Dominant layer
    // only, faded out over the last stretch — THE close-range depth cue
    // of the texturing brief; also its perf hot spot, hence the knob.
    f32 pomDistance { 12.0f };
    // Anti-repetition variety strength (0 = off): a second albedo tap at
    // a non-harmonic frequency (0.37x) modulates each layer, so the 4 m
    // tile grid never repeats exactly. One extra fetch per visible layer.
    f32 splatVariety { 0.5f };
    // POM self-shadow strength (0 = off): sun-side occlusion taps darken
    // the crevices inside the POM reach.
    f32 pomShadowStrength { 0.6f };
    // Parallax relief depth (uv units): how far the POM march displaces.
    f32 pomDepth { 0.03f };
    // Cooked terrain material arrays (.mtex asset guids, one per map —
    // docs/AUDIT/TERRAIN-TEXTURING.md §4). 0 = procedural splat tiles.
    // Vulkan-only path (caps.textureCompressionBC).
    core::Guid terrainAlbedoArray;
    core::Guid terrainNormalArray;
    core::Guid terrainOrmArray;
    core::Guid terrainHeightArray;
    // Fog / atmosphere.
    f32 fogDensity { 0.0012f };
    f32 fogHeightFalloff { 0.02f };
    f32 fogLowBoost { 1.6f };
    f32 fogStart { 450.0f };
    // Sun single-scatter phase exponent (docs/RENDERING.md V1): how
    // tightly the fog's sun glow hugs the sun direction. Strength is
    // per-weather (WeatherForm::fogSunScatter).
    f32 fogSunPhase { 4.1f };
    // Fog ceiling falloff baseline (1/m; per-weather override exists).
    f32 fogCeiling { 0.0035f };
    // Post processing.
    f32 exposure { 1.0f };
    f32 bloomIntensity { 0.35f };
    f32 godRayIntensity { 0.6f };
    f32 volumetricIntensity { 1.0f };
    // Unused (no screen-space AO in the engine —
    // grounding = terrain light map + contact shadows + baked vertex
    // AO). The field stays so existing records that set it keep
    // resolving; it is simply never read.
    f32 ssaoStrength { 0.0f };
    // Clouds.
    f32 cloudCoverage { 0.3f };
    f32 cloudShadowStrength { 0.95f };
    f32 cloudHeight { 950.0f };   // meters
    f32 cloudScale { 0.0011f };   // pattern frequency (1/m)
    // Interior ambient: the interior-mode base light; moddable per §5.
    Vec3 interiorAmbient { 0.16f, 0.15f, 0.14f };
    // H1 (docs/RENDERING.md): coupling of the interior ambient to the
    // outside (sun elevation x weather) — 0 = constant, 1 = full Helios.
    f32 interiorDaylightWeight { 0.6f };
    // Analytical grading:
    // applied in tonemap between ACES and gamma; the scene's A/B toggle
    // sends neutral values (0 / 0 / 1) when off.
    f32 gradeVibrance { 0.3f };   // weighted saturation boost
    f32 gradeSplitTone { 0.35f }; // cool shadows / warm highlights
    f32 gradeContrast { 1.06f };  // pivot 0.5; 1 = neutral
    // Auto-exposure bounds:
    // the adapted exposure is clamped to [min, max]; the Exposure slider
    // becomes the EV bias on top.
    f32 autoExposureMin { 0.4f };
    f32 autoExposureMax { 2.5f };
    // Stylized lighting ramp (stylized.glsl; defaults = the shipped
    // halisavakis cel model). Diffuse: two smoothstep edges (terminator
    // half-tone, then full light) mixed by halfTone; shadow: the CSM
    // snap window plus a floor that keeps shadow pools readable.
    f32 stylizedDiffuseEdge0Start { 0.02f };
    f32 stylizedDiffuseEdge0End { 0.09f };
    f32 stylizedDiffuseEdge1Start { 0.32f };
    f32 stylizedDiffuseEdge1End { 0.40f };
    f32 stylizedHalfTone { 0.6f };
    // CSM snap window: the BotW flat-pool look. Inert while the panel's
    // stylized A/B is off (the branch default — WorldRenderer::stylizedUi).
    f32 stylizedShadowStart { 0.45f };
    f32 stylizedShadowEnd { 0.55f };
    f32 stylizedShadowFloor { 0.0f };
    // Cel specular band on characters/props (mesh/skinned.frag).
    f32 stylizedSpecStrength { 0.35f };
    f32 stylizedSpecThreshold { 0.35f };
    f32 stylizedSpecExponent { 24.0f };
    // CSM texels per cascade side (1024/2048/4096 — the panel's combo).
    i32 shadowResolution { 2048 };
    // Planar-reflection resolution as a fraction of the window
    // (0.25..0.5) — mirror sharpness vs fill rate.
    f32 reflectionScale { 0.5f };
    // Clustered forward (docs/RENDERING.md §5): per-cluster light lists
    // replace the per-pixel 24-light loop and unlock the full budget.
    bool clusteredLights { true };
    // V4/H4 froxel fog: the A/B against the 2D march, and the interiors'
    // uniform dust density (window projectors carve their slabs in it).
    bool froxelFog { true };
    // Temporal accumulation strength: fraction of the current sample per
    // frame (0.1 = ~90% history; 1 = off).
    f32 froxelTemporalBlend { 0.1f };
    // Dust wisps: 0 = uniform dust, 1 = fully sparse drifting sheets.
    f32 froxelDustNoise { 0.6f };
    f32 interiorDustDensity { 0.025f };
    // Vegetation draw budget, moddable + live-tunable (the vegetation
    // pass is a major GPU cost driver). Radii in 64 m chunks.
    i32 vegViewRadius { 12 };       // resident/drawn ring
    i32 vegHighDetailRadius { 2 };  // full-detail canopies inside (~128 m)
    // 80-face twins inside; 20-face ultra lobes beyond.
    i32 vegLowDetailRadius { 4 };
    // Grass meadow — mirrors render::GrassRenderTuning field for field
    // (defaults MUST match; splatUvScale above feeds the scatter bake).
    // Tints multiply the ground albedo inherited at each blade's root.
    f32 grassBladeHeight { 0.95f };
    f32 grassBladeHalfWidth { 0.03f };
    f32 grassDetailNear { 12.5f };
    f32 grassDetailFar { 25.0f };
    f32 grassThinStart { 10.0f };
    f32 grassThinEnd { 70.0f };
    f32 grassFarDensity { 0.20f };
    f32 grassWidthCompensation { 1.7f };
    f32 grassFadeStart { 140.0f };
    f32 grassFadeEnd { 190.0f };
    Vec3 grassBaseTint { 1.0f, 1.0f, 1.0f };
    Vec3 grassTipTint { 1.0f, 1.0f, 1.0f };
    f32 grassRootAo { 1.0f };
    f32 grassSheen { 0.5f };
    f32 grassBladeNormals { 0.0f };
    f32 grassBrightMin { 1.0f };
    f32 grassBrightMax { 1.0f };
    f32 grassMiddleDarken { 0.0f };
    f32 grassBackscatter { 0.0f };
    // Scatter half (render::GrassScatterTuning — re-bakes on change).
    f32 grassSpacing { 0.15f };
    f32 grassPatchBroadScale { 21.0f };
    f32 grassPatchDetailScale { 6.0f };
    f32 grassPatchThresholdLo { 0.47f };
    f32 grassPatchThresholdHi { 0.60f };
    f32 grassPresenceLo { 0.08f };
    f32 grassPresenceHi { 0.40f };
    f32 grassMaterialCutoff { 0.72f };
    // Ground mist structure (mist.frag; density/coverage are per-weather
    // — WeatherForm). Scales in 1/m, lift/reach in meters.
    bool mistEnabled { true };
    f32 mistReach { 1200.0f };
    f32 mistLift { 49.0f };
    f32 mistCoverageSoftness { 0.6f };
    f32 mistCoverageScale { 0.0035f };
    f32 mistErosionScale { 0.02f };
    f32 mistErosionStrength { 0.2f };
    f32 mistSunBoost { 10.0f };
    f32 mistSunLobe { 0.95f };     // forward HG g (silver-lining tightness)
    f32 mistBackscatter { 0.8f };  // back-lobe weight
    f32 mistAmbientGain { 1.25f }; // mist body brightness vs the sun beam
    f32 mistShadowFloor { 0.6f };  // ambient kept in CSM/cloud shadow
    bool mistNoiseTexture { true }; // baked volume vs analytic fbm3
    f32 mistDetailDropout { 400.0f };
    i32 mistSteps { 16 };
    f32 mistTemporalBlend { 0.15f };
    f32 mistPuffiness { 0.5f }; // fractal edge florets
    // Volumetric sky clouds (§8): shape + light; coverage/height/scale
    // stay the per-weather cloud fields above.
    bool skyCloudsVolumetric { true };
    f32 skyCloudThickness { 440.0f };
    f32 skyCloudDensity { 0.065f };
    f32 skyCloudErosion { 0.31f };
    f32 skyCloudSunGain { 19.9f };  // body (multi-octave) gain
    f32 skyCloudSunLobe { 0.3f };   // body HG g
    f32 skyCloudAmbientGain { 0.9f };
    f32 skyCloudLiningGain { 30.2f }; // direct-transmission silver lining
    f32 skyCloudLiningLobe { 0.8f };
    f32 skyCloudPowder { 1.0f };
    f32 skyCloudThicknessSpread { 3.4f }; // thickness <-> coverage
    f32 skyCloudPuffiness { 0.5f };       // fractal edge erosion
    f32 skyCloudRimGain { 25.0f };  // view-thin silhouette glow
    f32 skyCloudRimLobe { 0.75f };
    f32 skyCloudBaseDark { 7.4f }; // storm-base ambient occlusion
    // Unused (the under-cloud sky-ray tail was tried 2026-07-30 and
    // removed: after its containments it only lit the air BETWEEN the
    // clouds — the ground-to-cloud shafts are the froxels' job). The
    // field stays so records that set it keep resolving.
    f32 skyCloudRays { 0.0f };

    // Terrain shape, continued (appended — reflection lists only grow).
    // Defaults MUST match render::TerrainParams.
    i32 terrainOctaves { 5 };
    f32 terrainLacunarity { 2.0f };
    f32 terrainGain { 0.5f };
    f32 mountainMaskLow { 0.45f };
    f32 mountainMaskHigh { 0.75f };
    // Sandbox mode: infinite generated terrain — super-tiles baked and
    // cached around the player (seed = terrainSeed), analytic macro as
    // the far fallback. The MAIN MENU drives this at runtime (story /
    // sandbox); true here forces sandbox from data (headless/mods).
    bool sandboxTerrain { false };
    // Snow altitude of the sandbox world (its peaks top ~1600 m; the
    // story world keeps `snowLine` above).
    f32 sandboxSnowLine { 950.0f };
    // Named tree-type records (editorId in the TreeCreationScene
    // library) assigned to the scatter's variant-slot partition:
    // broadleaf = the low slots, conifer = the high ones (the altitude
    // bands pick among them). Empty = every slot follows the default
    // *TreeTuningForm records.
    str broadleafTreeType {};
    str coniferTreeType {};
    str bushTreeType {};
    // The tree line as a fraction of the snow line (scatter + far
    // fringe); real treelines sit well under the permanent snow.
    f32 treeLineFactor { 0.82f };
    // Seasons (until a season system drives them live): autumn color
    // blend and deciduous leaf fall, 0..1 — species weight them by
    // their own `seasonality`.
    f32 seasonAutumn { 0.0f };
    f32 seasonLeafFall { 0.0f };
    // Sandbox elevation recurve (MacroParams::recurve*): curve outputs
    // at normalized inputs 1/4, 1/2, 3/4 — 0.25/0.5/0.75 = identity.
    // NOTE: baked tiles cache by seed only; clear terrain-cache/<seed>
    // after changing these (same rule as seaLevel).
    f32 terrainRecurveLow { 0.25f };
    f32 terrainRecurveMid { 0.5f };
    f32 terrainRecurveHigh { 0.75f };

    REFLECT_BEGIN(LandscapeTuningForm, Form)
        REFLECT_FIELD(terrainSeed)
        REFLECT_FIELD(hillWavelength)
        REFLECT_FIELD(hillAmplitude)
        REFLECT_FIELD(mountainWavelength)
        REFLECT_FIELD(mountainAmplitude)
        REFLECT_FIELD(seaLevel)
        REFLECT_FIELD(terrainViewRadius)
        REFLECT_FIELD(farTerrain)
        REFLECT_FIELD(snowLine)
        REFLECT_FIELD(splatUvScale)
        REFLECT_FIELD(splatBlendDepth)
        REFLECT_FIELD(terrainTintStrength)
        REFLECT_FIELD(splatDetailFade)
        REFLECT_FIELD(pomDistance)
        REFLECT_FIELD(splatVariety)
        REFLECT_FIELD(pomShadowStrength)
        REFLECT_FIELD(pomDepth)
        REFLECT_FIELD(terrainAlbedoArray)
        REFLECT_FIELD(terrainNormalArray)
        REFLECT_FIELD(terrainOrmArray)
        REFLECT_FIELD(terrainHeightArray)
        REFLECT_FIELD(fogDensity)
        REFLECT_FIELD(fogHeightFalloff)
        REFLECT_FIELD(fogLowBoost)
        REFLECT_FIELD(fogStart)
        REFLECT_FIELD(fogSunPhase)
        REFLECT_FIELD(fogCeiling)
        REFLECT_FIELD(exposure)
        REFLECT_FIELD(bloomIntensity)
        REFLECT_FIELD(godRayIntensity)
        REFLECT_FIELD(volumetricIntensity)
        REFLECT_FIELD(ssaoStrength)
        REFLECT_FIELD(cloudCoverage)
        REFLECT_FIELD(cloudShadowStrength)
        REFLECT_FIELD(cloudHeight)
        REFLECT_FIELD(cloudScale)
        REFLECT_FIELD(interiorAmbient)
        REFLECT_FIELD(interiorDaylightWeight)
        REFLECT_FIELD(gradeVibrance)
        REFLECT_FIELD(gradeSplitTone)
        REFLECT_FIELD(gradeContrast)
        REFLECT_FIELD(autoExposureMin)
        REFLECT_FIELD(autoExposureMax)
        REFLECT_FIELD(stylizedDiffuseEdge0Start)
        REFLECT_FIELD(stylizedDiffuseEdge0End)
        REFLECT_FIELD(stylizedDiffuseEdge1Start)
        REFLECT_FIELD(stylizedDiffuseEdge1End)
        REFLECT_FIELD(stylizedHalfTone)
        REFLECT_FIELD(stylizedShadowStart)
        REFLECT_FIELD(stylizedShadowEnd)
        REFLECT_FIELD(stylizedShadowFloor)
        REFLECT_FIELD(stylizedSpecStrength)
        REFLECT_FIELD(stylizedSpecThreshold)
        REFLECT_FIELD(stylizedSpecExponent)
        REFLECT_FIELD(shadowResolution)
        REFLECT_FIELD(reflectionScale)
        REFLECT_FIELD(clusteredLights)
        REFLECT_FIELD(froxelFog)
        REFLECT_FIELD(froxelTemporalBlend)
        REFLECT_FIELD(froxelDustNoise)
        REFLECT_FIELD(interiorDustDensity)
        REFLECT_FIELD(vegViewRadius)
        REFLECT_FIELD(vegHighDetailRadius)
        REFLECT_FIELD(vegLowDetailRadius)
        REFLECT_FIELD(grassBladeHeight)
        REFLECT_FIELD(grassBladeHalfWidth)
        REFLECT_FIELD(grassDetailNear)
        REFLECT_FIELD(grassDetailFar)
        REFLECT_FIELD(grassThinStart)
        REFLECT_FIELD(grassThinEnd)
        REFLECT_FIELD(grassFarDensity)
        REFLECT_FIELD(grassWidthCompensation)
        REFLECT_FIELD(grassFadeStart)
        REFLECT_FIELD(grassFadeEnd)
        REFLECT_FIELD(grassBaseTint)
        REFLECT_FIELD(grassTipTint)
        REFLECT_FIELD(grassRootAo)
        REFLECT_FIELD(grassSheen)
        REFLECT_FIELD(grassBladeNormals)
        REFLECT_FIELD(grassBrightMin)
        REFLECT_FIELD(grassBrightMax)
        REFLECT_FIELD(grassMiddleDarken)
        REFLECT_FIELD(grassBackscatter)
        REFLECT_FIELD(grassSpacing)
        REFLECT_FIELD(grassPatchBroadScale)
        REFLECT_FIELD(grassPatchDetailScale)
        REFLECT_FIELD(grassPatchThresholdLo)
        REFLECT_FIELD(grassPatchThresholdHi)
        REFLECT_FIELD(grassPresenceLo)
        REFLECT_FIELD(grassPresenceHi)
        REFLECT_FIELD(grassMaterialCutoff)
        REFLECT_FIELD(mistEnabled)
        REFLECT_FIELD(mistReach)
        REFLECT_FIELD(mistLift)
        REFLECT_FIELD(mistCoverageSoftness)
        REFLECT_FIELD(mistCoverageScale)
        REFLECT_FIELD(mistErosionScale)
        REFLECT_FIELD(mistErosionStrength)
        REFLECT_FIELD(mistSunBoost)
        REFLECT_FIELD(mistSunLobe)
        REFLECT_FIELD(mistBackscatter)
        REFLECT_FIELD(mistAmbientGain)
        REFLECT_FIELD(mistShadowFloor)
        REFLECT_FIELD(mistNoiseTexture)
        REFLECT_FIELD(mistDetailDropout)
        REFLECT_FIELD(mistSteps)
        REFLECT_FIELD(mistTemporalBlend)
        REFLECT_FIELD(mistPuffiness)
        REFLECT_FIELD(skyCloudsVolumetric)
        REFLECT_FIELD(skyCloudThickness)
        REFLECT_FIELD(skyCloudDensity)
        REFLECT_FIELD(skyCloudErosion)
        REFLECT_FIELD(skyCloudSunGain)
        REFLECT_FIELD(skyCloudSunLobe)
        REFLECT_FIELD(skyCloudAmbientGain)
        REFLECT_FIELD(skyCloudLiningGain)
        REFLECT_FIELD(skyCloudLiningLobe)
        REFLECT_FIELD(skyCloudPowder)
        REFLECT_FIELD(skyCloudThicknessSpread)
        REFLECT_FIELD(skyCloudPuffiness)
        REFLECT_FIELD(skyCloudRimGain)
        REFLECT_FIELD(skyCloudRimLobe)
        REFLECT_FIELD(skyCloudBaseDark)
        REFLECT_FIELD(skyCloudRays)
        REFLECT_FIELD(terrainOctaves)
        REFLECT_FIELD(terrainLacunarity)
        REFLECT_FIELD(terrainGain)
        REFLECT_FIELD(mountainMaskLow)
        REFLECT_FIELD(mountainMaskHigh)
        REFLECT_FIELD(sandboxTerrain)
        REFLECT_FIELD(sandboxSnowLine)
        REFLECT_FIELD(broadleafTreeType)
        REFLECT_FIELD(coniferTreeType)
        REFLECT_FIELD(bushTreeType)
        REFLECT_FIELD(treeLineFactor)
        REFLECT_FIELD(seasonAutumn)
        REFLECT_FIELD(seasonLeafFall)
        REFLECT_FIELD(terrainRecurveLow)
        REFLECT_FIELD(terrainRecurveMid)
        REFLECT_FIELD(terrainRecurveHigh)
    REFLECT_END()
};

// Tree builder: the generation knobs of each procedural tree
// type, as ordinary records (§5 — a mod retunes a species in pure TOML).
// Mirrors render::LobeTreeParams field-for-field; the scene maps Form ->
// flat engine params (engine/* never includes data/*, §4).
struct LobeTreeTuningForm : Form {
    f32 trunkHeightMin { 4.2f };
    f32 trunkHeightMax { 6.1f };
    f32 trunkRadiusMin { 0.17f };
    f32 trunkRadiusMax { 0.24f };
    f32 trunkTaper { 0.42f };
    f32 lean { 0.16f };
    i32 branchCountMin { 3 };
    i32 branchCountMax { 5 };
    f32 branchLengthMin { 0.9f };
    f32 branchLengthMax { 1.7f };
    f32 crownLobeRadiusMin { 0.85f };
    f32 crownLobeRadiusMax { 1.25f };
    f32 branchLobeRadiusMin { 0.60f };
    f32 branchLobeRadiusMax { 0.98f };
    f32 lobeFlatten { 0.85f };
    f32 normalSpherize { 0.4f };

    REFLECT_BEGIN(LobeTreeTuningForm, Form)
        REFLECT_FIELD(trunkHeightMin)
        REFLECT_FIELD(trunkHeightMax)
        REFLECT_FIELD(trunkRadiusMin)
        REFLECT_FIELD(trunkRadiusMax)
        REFLECT_FIELD(trunkTaper)
        REFLECT_FIELD(lean)
        REFLECT_FIELD(branchCountMin)
        REFLECT_FIELD(branchCountMax)
        REFLECT_FIELD(branchLengthMin)
        REFLECT_FIELD(branchLengthMax)
        REFLECT_FIELD(crownLobeRadiusMin)
        REFLECT_FIELD(crownLobeRadiusMax)
        REFLECT_FIELD(branchLobeRadiusMin)
        REFLECT_FIELD(branchLobeRadiusMax)
        REFLECT_FIELD(lobeFlatten)
        REFLECT_FIELD(normalSpherize)
    REFLECT_END()
};

// Mirrors render::ColonizedTreeParams (the Runions/SDF-card tree).
struct ColonizedTreeTuningForm : Form {
    // Wood look: MAX ring vertices (trunk; LODs derive), how much of
    // the growth trajectory's bends survive decimation (0 = straight
    // runs), Catmull-Rom points per segment rounding the elbows, kink
    // noise on the kept trajectory points, angular irregularity of the
    // tube faces, and the ring-count HALVING floor for thin branches
    // (fraction of tubeSides; 1 = constant count; pick an even
    // tubeSides for clean halvings).
    i32 tubeSides { 12 };
    f32 curvePreserve { 0.0f };
    i32 curveSubdiv { 0 };
    f32 pathJitter { 0.0f };
    f32 ringIrregularity { 0.0f };
    f32 sideMinFraction { 0.5f };
    f32 segment { 0.28f };
    f32 killDistance { 0.70f };
    i32 attractorCount { 350 };
    f32 pipeExponent { 2.6f };
    f32 tropism { 0.14f };
    f32 trunkBaseMin { 1.6f };
    f32 trunkBaseMax { 2.5f };
    f32 crownHeightMin { 2.6f };
    f32 crownHeightMax { 3.8f };
    f32 crownRadiusMin { 1.9f };
    f32 crownRadiusMax { 3.0f };
    // Conifer habit (0 = broadleaf): cone profile, apical leader,
    // whorled shelves, branch-riding foliage sprays.
    f32 crownTaper { 0.0f };
    f32 leaderBias { 0.0f };
    f32 lateralFlatten { 0.0f };
    f32 sprayFoliage { 0.0f };
    f32 tipBallRadius { 0.95f };
    f32 tipOrderFalloff { 0.78f };
    f32 tipBallMin { 0.30f };
    i32 leafStyle { 0 };   // leaf-mask atlas slot (0..7)
    i32 leafShape { 0 };   // 0 ellipse, 1 needles, 2 rounded, 3 lobed,
                           // 4 serrated
    Vec3 autumnTint { 0.62f, 0.30f, 0.08f };
    f32 seasonality { 1.0f }; // 0 = evergreen (no autumn, no leaf fall)
    f32 smoothK { 0.7f };
    f32 cardHalfSizeMin { 0.084f };
    f32 cardHalfSizeMax { 0.144f };
    f32 densityGradient { 3.0f };
    f32 foliageDensity { 2.5f };
    i32 leafCount { 60 };
    f32 leafSizeMin { 0.10f };
    f32 leafSizeMax { 0.25f };
    f32 leafSolidStart { 4.0f };
    f32 leafSolidEnd { 7.0f };

    REFLECT_BEGIN(ColonizedTreeTuningForm, Form)
        REFLECT_FIELD(tubeSides)
        REFLECT_FIELD(curvePreserve)
        REFLECT_FIELD(curveSubdiv)
        REFLECT_FIELD(pathJitter)
        REFLECT_FIELD(ringIrregularity)
        REFLECT_FIELD(sideMinFraction)
        REFLECT_FIELD(segment)
        REFLECT_FIELD(killDistance)
        REFLECT_FIELD(attractorCount)
        REFLECT_FIELD(pipeExponent)
        REFLECT_FIELD(tropism)
        REFLECT_FIELD(trunkBaseMin)
        REFLECT_FIELD(trunkBaseMax)
        REFLECT_FIELD(crownHeightMin)
        REFLECT_FIELD(crownHeightMax)
        REFLECT_FIELD(crownRadiusMin)
        REFLECT_FIELD(crownRadiusMax)
        REFLECT_FIELD(crownTaper)
        REFLECT_FIELD(leaderBias)
        REFLECT_FIELD(lateralFlatten)
        REFLECT_FIELD(sprayFoliage)
        REFLECT_FIELD(tipBallRadius)
        REFLECT_FIELD(tipOrderFalloff)
        REFLECT_FIELD(tipBallMin)
        REFLECT_FIELD(leafStyle)
        REFLECT_FIELD(leafShape)
        REFLECT_FIELD(autumnTint)
        REFLECT_FIELD(seasonality)
        REFLECT_FIELD(smoothK)
        REFLECT_FIELD(cardHalfSizeMin)
        REFLECT_FIELD(cardHalfSizeMax)
        REFLECT_FIELD(densityGradient)
        REFLECT_FIELD(foliageDensity)
        REFLECT_FIELD(leafCount)
        REFLECT_FIELD(leafSizeMin)
        REFLECT_FIELD(leafSizeMax)
        REFLECT_FIELD(leafSolidStart)
        REFLECT_FIELD(leafSolidEnd)
    REFLECT_END()
};

// Radiance-cascades GI tuning (mirrors render::RcTuning; §5 precedent:
// StatsTuningForm). One record, canonical guid, resolved by
// resolveRcTuning(); the scene maps it onto the renderer's live struct.
// `technique`: 0 = Classic, 1 = RadianceCascades.
struct RcTuningForm : Form {
    i32 resolution { 64 };
    f32 fineVoxel { 0.5f };
    f32 coarseVoxel { 2.0f };
    i32 cascadeCount { 5 };
    i32 updateInterval { 1 };
    i32 technique { 1 };
    f32 intensity { 0.7f };
    f32 skyFactor { 0.6f };
    f32 emitterBoost { 1.0f };
    f32 lightSplatBounce { 0.35f };
    f32 bounceFeedback { 0.5f };
    bool pipelined { true };
    bool asyncCompute { true };
    bool rcOnlyLights { false };
    f32 interval0 { 1.0f };
    f32 edgeFade { 8.0f };
    f32 bandCount { 0.0f };
    f32 bandAa { 0.3f };
    f32 giFloor { 0.7f };
    bool intervalExtension { false };

    REFLECT_BEGIN(RcTuningForm, Form)
        REFLECT_FIELD(resolution)
        REFLECT_FIELD(fineVoxel)
        REFLECT_FIELD(coarseVoxel)
        REFLECT_FIELD(cascadeCount)
        REFLECT_FIELD(updateInterval)
        REFLECT_FIELD(technique)
        REFLECT_FIELD(intensity)
        REFLECT_FIELD(skyFactor)
        REFLECT_FIELD(emitterBoost)
        REFLECT_FIELD(lightSplatBounce)
        REFLECT_FIELD(bounceFeedback)
        REFLECT_FIELD(pipelined)
        REFLECT_FIELD(asyncCompute)
        REFLECT_FIELD(rcOnlyLights)
        REFLECT_FIELD(interval0)
        REFLECT_FIELD(edgeFade)
        REFLECT_FIELD(bandCount)
        REFLECT_FIELD(bandAa)
        REFLECT_FIELD(giFloor)
        REFLECT_FIELD(intervalExtension)
    REFLECT_END()
};

// One weather state: a full parameter set the scene
// crossfades to. Ordinary records — a mod adds a weather type or retunes
// one in pure TOML (§5).
struct WeatherForm : Form {
    i32 sortOrder { 0 };  // dropdown position
    // Clouds.
    f32 cloudCoverage { 0.3f };
    f32 cloudScale { 0.0011f };   // pattern frequency (1/m)
    f32 cloudHeight { 950.0f };   // meters
    f32 cloudShadowStrength { 0.95f };
    // Fog / atmosphere.
    f32 fogDensity { 0.0014f };
    f32 fogHeightFalloff { 0.02f };
    f32 fogLowBoost { 1.6f };
    f32 fogStart { 300.0f };
    // Sun single-scatter strength in the fog (docs/RENDERING.md V1):
    // morning mist pushes it, overcast kills it.
    f32 fogSunScatter { 0.5f };
    // Fog ceiling falloff (1/m): how fast the fog layer thins with
    // altitude — clear weathers free the sky, overcast keeps a dome.
    f32 fogCeiling { 0.0035f };
    // Light grading (SkySystem::Weather).
    f32 sunIntensity { 1.0f };
    f32 ambientIntensity { 1.0f };
    f32 saturation { 1.0f };
    f32 warmth { 0.0f };  // reddens dawn/dusk (haze)
    // Post.
    f32 volumetricIntensity { 1.0f };
    f32 godRayIntensity { 0.6f };
    f32 bloomIntensity { 0.35f };
    // Wind / water.
    f32 windStrength { 1.0f };  // sway amplitude + drift/wave speed
    f32 waveChop { 1.0f };      // water surface roughness
    // Horizon cumulonimbus towers, 0-1 (Storm = 1).
    f32 stormFront { 0.0f };
    // Rain streaks + wetness, 0-1.
    f32 rainIntensity { 0.0f };
    // Ground mist (mist.frag): extinction (1/m, 0 = none) and how much
    // of the valley network holds mist (0-1). Non-zero defaults: the
    // erasing mist is the world's baseline — a record only sets these
    // to DEVIATE from it.
    f32 mistDensity { 0.6f };
    f32 mistCoverage { 0.4f };

    REFLECT_BEGIN(WeatherForm, Form)
        REFLECT_FIELD(sortOrder)
        REFLECT_FIELD(cloudCoverage)
        REFLECT_FIELD(cloudScale)
        REFLECT_FIELD(cloudHeight)
        REFLECT_FIELD(cloudShadowStrength)
        REFLECT_FIELD(fogDensity)
        REFLECT_FIELD(fogHeightFalloff)
        REFLECT_FIELD(fogLowBoost)
        REFLECT_FIELD(fogStart)
        REFLECT_FIELD(fogSunScatter)
        REFLECT_FIELD(fogCeiling)
        REFLECT_FIELD(sunIntensity)
        REFLECT_FIELD(ambientIntensity)
        REFLECT_FIELD(saturation)
        REFLECT_FIELD(warmth)
        REFLECT_FIELD(volumetricIntensity)
        REFLECT_FIELD(godRayIntensity)
        REFLECT_FIELD(bloomIntensity)
        REFLECT_FIELD(windStrength)
        REFLECT_FIELD(waveChop)
        REFLECT_FIELD(stormFront)
        REFLECT_FIELD(rainIntensity)
        REFLECT_FIELD(mistDensity)
        REFLECT_FIELD(mistCoverage)
    REFLECT_END()
};

// Registers the landscape Form types. Call before loading landscape.toml.
void registerLandscapeFormTypes(FormTypeRegistry& registry);

// Resolves the tuning from the database (canonical guid), or defaults.
LandscapeTuningForm resolveLandscapeTuning(const FormDatabase& forms);

// Tree builder: fixed-GUID singletons like the landscape tuning —
// defaults when the record is absent (older plugin stacks).
LobeTreeTuningForm resolveLobeTreeTuning(const FormDatabase& forms);
ColonizedTreeTuningForm resolveColonizedTreeTuning(const FormDatabase& forms);
RcTuningForm resolveRcTuning(const FormDatabase& forms);

// Canonical guids of the singleton tuning records — the render panels'
// "Save" button patches THESE records (into the render-tuning overlay
// plugin), never a copy.
const core::Guid& landscapeTuningGuid();
const core::Guid& lobeTreeTuningGuid();
const core::Guid& colonizedTreeTuningGuid();
const core::Guid& rcTuningGuid();

// Every WeatherForm in the database, sorted by sortOrder — feeds the
// weather dropdown. Empty if the plugin ships none.
vector<WeatherForm> resolveWeatherForms(const FormDatabase& forms);

} // namespace data
