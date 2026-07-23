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
    f32 seaLevel { 21.0f };
    // Terrain materials.
    f32 snowLine { 165.0f };     // meters
    f32 splatUvScale { 0.25f };  // tiles per meter
    // Fog / atmosphere.
    f32 fogDensity { 0.0014f };
    f32 fogHeightFalloff { 0.02f };
    f32 fogLowBoost { 1.6f };
    f32 fogStart { 300.0f };
    // Sun single-scatter phase exponent (docs/VOLUMETRIC.md V1): how
    // tightly the fog's sun glow hugs the sun direction. Strength is
    // per-weather (WeatherForm::fogSunScatter).
    f32 fogSunPhase { 8.0f };
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
    f32 cloudCoverage { 0.38f };
    f32 cloudShadowStrength { 0.7f };
    f32 cloudHeight { 780.0f };   // meters
    f32 cloudScale { 0.0011f };   // pattern frequency (1/m)
    // Interior ambient: the interior-mode base light; moddable per §5.
    Vec3 interiorAmbient { 0.16f, 0.15f, 0.14f };
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
    f32 stylizedShadowStart { 0.45f };
    f32 stylizedShadowEnd { 0.55f };
    f32 stylizedShadowFloor { 0.0f };
    // CSM texels per cascade side (1024/2048/4096 — the panel's combo).
    i32 shadowResolution { 2048 };
    // Vegetation draw budget, moddable + live-tunable (the vegetation
    // pass is a major GPU cost driver). Radii in 64 m chunks.
    i32 vegViewRadius { 12 };       // resident/drawn ring
    i32 vegHighDetailRadius { 2 };  // full-detail canopies inside (~128 m)
    // 80-face twins inside; 20-face ultra lobes beyond.
    i32 vegLowDetailRadius { 4 };

    REFLECT_BEGIN(LandscapeTuningForm, Form)
        REFLECT_FIELD(terrainSeed)
        REFLECT_FIELD(hillWavelength)
        REFLECT_FIELD(hillAmplitude)
        REFLECT_FIELD(mountainWavelength)
        REFLECT_FIELD(mountainAmplitude)
        REFLECT_FIELD(seaLevel)
        REFLECT_FIELD(snowLine)
        REFLECT_FIELD(splatUvScale)
        REFLECT_FIELD(fogDensity)
        REFLECT_FIELD(fogHeightFalloff)
        REFLECT_FIELD(fogLowBoost)
        REFLECT_FIELD(fogStart)
        REFLECT_FIELD(fogSunPhase)
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
        REFLECT_FIELD(shadowResolution)
        REFLECT_FIELD(vegViewRadius)
        REFLECT_FIELD(vegHighDetailRadius)
        REFLECT_FIELD(vegLowDetailRadius)
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
    f32 tipBallRadius { 0.95f };
    f32 tipOrderFalloff { 0.78f };
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
        REFLECT_FIELD(tipBallRadius)
        REFLECT_FIELD(tipOrderFalloff)
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
    f32 bounceFeedback { 0.5f };
    bool rcOnlyLights { false };
    f32 interval0 { 1.0f };
    f32 edgeFade { 8.0f };
    f32 bandStops { 0.85f };
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
        REFLECT_FIELD(bounceFeedback)
        REFLECT_FIELD(rcOnlyLights)
        REFLECT_FIELD(interval0)
        REFLECT_FIELD(edgeFade)
        REFLECT_FIELD(bandStops)
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
    f32 cloudCoverage { 0.38f };
    f32 cloudScale { 0.0011f };   // pattern frequency (1/m)
    f32 cloudHeight { 780.0f };   // meters
    f32 cloudShadowStrength { 0.7f };
    // Fog / atmosphere.
    f32 fogDensity { 0.0014f };
    f32 fogHeightFalloff { 0.02f };
    f32 fogLowBoost { 1.6f };
    f32 fogStart { 300.0f };
    // Sun single-scatter strength in the fog (docs/VOLUMETRIC.md V1):
    // morning mist pushes it, overcast kills it.
    f32 fogSunScatter { 0.5f };
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
