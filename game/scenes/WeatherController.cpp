#include "game/scenes/WeatherController.hpp"

#include <glm/glm.hpp>

namespace game {

namespace {

// THE AtmosphereParams <-> WeatherForm field mapping, in ONE place:
// capture, apply and the crossfade all walk this table, so a new weather
// field is added exactly here (forgetting one of three hand-written
// lists is how fields silently stop saving). Two fields are spelled
// differently on the two structs: cloudShadow <-> cloudShadowStrength,
// volumetric <-> volumetricIntensity. A reflection-driven version (no
// hand-listed fields) is a possible later step.
struct WeatherLane {
    f32 render::AtmosphereParams::* atmos;
    f32 data::WeatherForm::* form;
};
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
    for (const WeatherLane& lane : kWeatherLanes) {
        w.*lane.form = a.*lane.atmos;
    }
    return w;
}

void applyTo(render::AtmosphereParams& a, const data::WeatherForm& w) {
    for (const WeatherLane& lane : kWeatherLanes) {
        a.*lane.atmos = w.*lane.form;
    }
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
    for (const WeatherLane& lane : kWeatherLanes) {
        atmos.*lane.atmos = glm::mix(from_.*lane.form, to.*lane.form, t);
    }
}

} // namespace game
