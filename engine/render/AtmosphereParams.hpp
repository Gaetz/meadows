#pragma once

#include "engine/core/Defines.hpp"

namespace render {

// The scene's current atmospheric render state: the 19 sky/fog/weather-driven
// parameters that render() consumes and that BOTH the manual sliders and the
// weather crossfade write into. Grouped out of LandscapeScene so a
// WeatherController can own the transition without reaching into the
// scene's members. Field names and defaults mirror the former `*Ui` members;
// they also mirror WeatherForm's fields (capture/apply convert between the two).
struct AtmosphereParams {
    f32 cloudCoverage { 0.3f };
    f32 cloudShadow { 0.95f };
    f32 bloomIntensity { 0.35f };
    f32 godRayIntensity { 0.6f };
    f32 volumetric { 1.0f };
    f32 fogDensity { 0.0012f };
    f32 fogHeightFalloff { 0.02f };
    f32 fogLowBoost { 1.6f };
    f32 fogStart { 450.0f };
    // Fog sun single-scatter (docs/RENDERING.md V1): strength rides the
    // weather crossfade; the phase exponent is global tuning.
    f32 fogSunScatter { 0.5f };
    f32 fogSunPhase { 4.1f };
    // Fog ceiling falloff (1/m above sea level): how fast the fog layer
    // thins with altitude — high = clear sky, low = grey dome.
    f32 fogCeiling { 0.0035f };
    f32 cloudHeight { 520.0f };
    f32 cloudScale { 0.0011f };
    f32 sunIntensity { 1.0f };
    f32 ambientIntensity { 1.0f };
    f32 saturation { 1.0f };
    f32 warmth { 0.0f };
    f32 windStrength { 1.0f };
    f32 waveChop { 1.0f };
    f32 stormFront { 0.0f };
    f32 rainIntensity { 0.0f };
    // Ground mist (mist.frag): extinction (1/m, 0 = none) + how much of
    // the valley network holds mist. Non-zero defaults: the erasing
    // mist is the world's baseline, weathers modulate it.
    f32 mistDensity { 0.6f };
    f32 mistCoverage { 0.4f };
};

} // namespace render
