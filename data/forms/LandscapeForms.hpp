#pragma once

#include "data/forms/Form.hpp"

// Landscape & weather tuning Forms. Moved from game/scenes (audit
// 2026-07-06): Forms must live in libs so TOOLS see them — the cooker and
// the Game DB editor register every family, and an exe-local Form type is
// invisible to both. Reflected type names are unqualified, so the move is
// invisible to TOML data.
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
    f32 hillWavelength { 400.0f };
    f32 hillAmplitude { 50.0f };
    f32 mountainWavelength { 1500.0f };
    f32 mountainAmplitude { 180.0f };
    f32 seaLevel { 14.0f };
    // Terrain materials.
    f32 snowLine { 110.0f };     // meters
    f32 splatUvScale { 0.25f };  // tiles per meter
    // Fog / atmosphere.
    f32 fogDensity { 0.0014f };
    f32 fogHeightFalloff { 0.02f };
    f32 fogLowBoost { 1.6f };
    f32 fogStart { 300.0f };
    // Post processing.
    f32 exposure { 1.0f };
    f32 bloomIntensity { 0.35f };
    f32 godRayIntensity { 0.6f };
    f32 volumetricIntensity { 1.0f };
    // RETIRED 2026-07-10 (screen-space AO removed from the engine —
    // grounding = terrain light map + contact shadows + baked vertex
    // AO). The field stays: reflected form layouts are APPEND-only
    // (binary ordinals); it is simply never read.
    f32 ssaoStrength { 0.0f };
    // Clouds.
    f32 cloudCoverage { 0.38f };
    f32 cloudShadowStrength { 0.7f };
    f32 cloudHeight { 520.0f };   // meters
    f32 cloudScale { 0.0011f };   // pattern frequency (1/m)
    // Interior ambient (chantier 6 B1, appended — ordinals stable):
    // replaces the hardcoded interior-mode constant; moddable per §5.
    Vec3 interiorAmbient { 0.16f, 0.15f, 0.14f };
    // Analytical grading (renderer brick 28, chantier 6 B3, appended):
    // applied in tonemap between ACES and gamma; the scene's A/B toggle
    // sends neutral values (0 / 0 / 1) when off.
    f32 gradeVibrance { 0.3f };   // weighted saturation boost
    f32 gradeSplitTone { 0.35f }; // cool shadows / warm highlights
    f32 gradeContrast { 1.06f };  // pivot 0.5; 1 = neutral
    // Auto-exposure bounds (renderer brick 29, chantier 6 B4, appended):
    // the adapted exposure is clamped to [min, max]; the Exposure slider
    // becomes the EV bias on top.
    f32 autoExposureMin { 0.4f };
    f32 autoExposureMax { 2.5f };
    // Vegetation draw budget (GPU-PERF P1, appended — ordinals stable):
    // the baseline put mainVeg at 1.8 ms — these were compile-time
    // constants; moddable + live-tunable now. Radii in 64 m chunks.
    i32 vegViewRadius { 12 };       // resident/drawn ring (dev pick)
    i32 vegHighDetailRadius { 5 };  // 320-face canopies inside (~320 m)
    // V8f (appended — ordinals stable): 80-face twins inside; 20-face
    // ultra lobes beyond (dev pick 2026-07-19 after the visual check).
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
        REFLECT_FIELD(vegViewRadius)
        REFLECT_FIELD(vegHighDetailRadius)
        REFLECT_FIELD(vegLowDetailRadius)
    REFLECT_END()
};

// One weather state (renderer brick 24): a full parameter set the scene
// crossfades to. Ordinary records — a mod adds a weather type or retunes
// one in pure TOML (§5).
struct WeatherForm : Form {
    i32 sortOrder { 0 };  // dropdown position
    // Clouds.
    f32 cloudCoverage { 0.38f };
    f32 cloudScale { 0.0011f };   // pattern frequency (1/m)
    f32 cloudHeight { 520.0f };   // meters
    f32 cloudShadowStrength { 0.7f };
    // Fog / atmosphere.
    f32 fogDensity { 0.0014f };
    f32 fogHeightFalloff { 0.02f };
    f32 fogLowBoost { 1.6f };
    f32 fogStart { 300.0f };
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
    // Brick 30 (appended): horizon cumulonimbus towers, 0-1 (Storm = 1).
    f32 stormFront { 0.0f };
    // Brick 31 (appended): rain streaks + wetness, 0-1.
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

// Every WeatherForm in the database, sorted by sortOrder — feeds the
// weather dropdown. Empty if the plugin ships none.
vector<WeatherForm> resolveWeatherForms(const FormDatabase& forms);

} // namespace data
