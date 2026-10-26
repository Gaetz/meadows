#include "game/scenes/WeatherController.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "engine/reflect/FieldLane.hpp"

namespace game {

namespace {

// THE AtmosphereParams <-> WeatherForm field mapping, in ONE place:
// capture, apply and the crossfade all walk this table, so a new weather
// field is added exactly here (forgetting one of three hand-written
// lists is how fields silently stop saving). Two fields are spelled
// differently on the two structs: cloudShadow <-> cloudShadowStrength,
// volumetric <-> volumetricIntensity. The global atmosphere baseline
// (LandscapeTuningForm) has its own table in RenderTuningIo — a
// different Form type, so the two tables cannot be shared. A
// reflection-driven version (no hand-listed fields) is a possible later
// step.
using WeatherLane =
    reflect::FieldLane<render::AtmosphereParams, data::WeatherForm, f32>;
constexpr WeatherLane kWeatherLanes[] = {
    { &render::AtmosphereParams::cloudCoverage,
      &data::WeatherForm::cloudCoverage },
    { &render::AtmosphereParams::cloudScale, &data::WeatherForm::cloudScale },
    { &render::AtmosphereParams::cloudHeight,
      &data::WeatherForm::cloudHeight },
    { &render::AtmosphereParams::cloudShadow,
      &data::WeatherForm::cloudShadowStrength },
    { &render::AtmosphereParams::fogDensity, &data::WeatherForm::fogDensity },
    { &render::AtmosphereParams::fogHeightFalloff,
      &data::WeatherForm::fogHeightFalloff },
    { &render::AtmosphereParams::fogLowBoost,
      &data::WeatherForm::fogLowBoost },
    { &render::AtmosphereParams::fogStart, &data::WeatherForm::fogStart },
    { &render::AtmosphereParams::fogSunScatter,
      &data::WeatherForm::fogSunScatter },
    { &render::AtmosphereParams::fogCeiling, &data::WeatherForm::fogCeiling },
    { &render::AtmosphereParams::sunIntensity,
      &data::WeatherForm::sunIntensity },
    { &render::AtmosphereParams::ambientIntensity,
      &data::WeatherForm::ambientIntensity },
    { &render::AtmosphereParams::saturation, &data::WeatherForm::saturation },
    { &render::AtmosphereParams::warmth, &data::WeatherForm::warmth },
    { &render::AtmosphereParams::volumetric,
      &data::WeatherForm::volumetricIntensity },
    { &render::AtmosphereParams::godRayIntensity,
      &data::WeatherForm::godRayIntensity },
    { &render::AtmosphereParams::bloomIntensity,
      &data::WeatherForm::bloomIntensity },
    { &render::AtmosphereParams::windStrength,
      &data::WeatherForm::windStrength },
    { &render::AtmosphereParams::waveChop, &data::WeatherForm::waveChop },
    { &render::AtmosphereParams::stormFront, &data::WeatherForm::stormFront },
    { &render::AtmosphereParams::rainIntensity,
      &data::WeatherForm::rainIntensity },
    { &render::AtmosphereParams::mistDensity,
      &data::WeatherForm::mistDensity },
    { &render::AtmosphereParams::mistCoverage,
      &data::WeatherForm::mistCoverage },
};

data::WeatherForm capture(const render::AtmosphereParams& a) {
    data::WeatherForm w;
    reflect::captureLanes(a, w, kWeatherLanes);
    return w;
}

void applyTo(render::AtmosphereParams& a, const data::WeatherForm& w) {
    reflect::applyLanes(a, w, kWeatherLanes);
}

} // namespace

void WeatherController::init(const data::FormDatabase& forms) {
    weathers_ = data::resolveWeatherForms(forms);
}

void WeatherController::beginTransition(
    i32 index, const render::AtmosphereParams& current) {
    selected_ = index;
    if (selected_ >= 0) {
        from_ = capture(current);
        blend_ = 0.0f;
    }
}

void WeatherController::update(render::AtmosphereParams& atmos, f32 dt) {
    // Every parameter slides from the captured start state to the selected
    // weather over `duration_` seconds.
    if (blend_ >= 1.0f || selected_ < 0 ||
        selected_ >= static_cast<i32>(weathers_.size())) {
        return;
    }
    blend_ = glm::min(blend_ + dt / glm::max(duration_, 0.01f), 1.0f);
    const data::WeatherForm& to = weathers_[selected_];
    const f32 t = glm::smoothstep(0.0f, 1.0f, blend_);
    // Two Forms feed each lane (from and to), so this walks the table
    // itself rather than through forEachLane.
    for (const WeatherLane& lane : kWeatherLanes) {
        atmos.*lane.param = glm::mix(from_.*lane.form, to.*lane.form, t);
    }
    // The wind's heading turns along the short arc (350 -> 10 is +20,
    // never a -340 sweep through the south).
    {
        const f32 a = from_.windDirectionDeg;
        const f32 b = to.windDirectionDeg;
        const f32 delta = std::fmod(b - a + 540.0f, 360.0f) - 180.0f;
        atmos.windDirectionDeg = a + delta * t;
    }
}

} // namespace game
