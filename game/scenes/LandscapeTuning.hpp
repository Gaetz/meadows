#pragma once

#include "data/forms/Form.hpp"

// Moddable tuning for the landscape renderer (§5), following the
// StatsTuningForm precedent: one Form resolved by canonical guid, defaults
// matching the values the renderer shipped with — behaviour is unchanged
// when the record is absent. Lives in game/ (not engine/render) so the
// renderer keeps zero data-layer dependency: the scene maps Form -> plain
// params at load.
//
// The record ships in game/data/base/landscape.toml — its own small plugin,
// the first slice of the base.toml split the dev wants. Mods patch any
// field, last writer wins per field.

namespace data {
class FormDatabase;
class FormTypeRegistry;
} // namespace data

namespace game {

struct LandscapeTuningForm : data::Form {
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
    f32 ssaoStrength { 0.7f };
    // Clouds.
    f32 cloudCoverage { 0.38f };
    f32 cloudShadowStrength { 0.7f };
    f32 cloudHeight { 520.0f };   // meters
    f32 cloudScale { 0.0011f };   // pattern frequency (1/m)

    REFLECT_BEGIN(LandscapeTuningForm, data::Form)
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
    REFLECT_END()
};

// Registers the landscape Form types. Call before loading landscape.toml.
void registerLandscapeFormTypes(data::FormTypeRegistry& registry);

// Resolves the tuning from the database (canonical guid), or defaults.
LandscapeTuningForm resolveLandscapeTuning(const data::FormDatabase& forms);

} // namespace game
