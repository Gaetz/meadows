#include "game/scenes/WeatherController.hpp"

#include <glm/glm.hpp>

namespace game {

namespace {

// AtmosphereParams <-> WeatherForm field copy, moved verbatim from the scene's
// captureCurrentWeather/applyWeather. Two fields are spelled differently on the
// two structs: cloudShadow<->cloudShadowStrength, volumetric<->volumetric-
// Intensity. A reflection-driven version (no hand-listed fields) is a later
// brick (audit U4-8).
data::WeatherForm capture(const AtmosphereParams& a) {
    data::WeatherForm w;
    w.cloudCoverage = a.cloudCoverage;
    w.cloudScale = a.cloudScale;
    w.cloudHeight = a.cloudHeight;
    w.cloudShadowStrength = a.cloudShadow;
    w.fogDensity = a.fogDensity;
    w.fogHeightFalloff = a.fogHeightFalloff;
    w.fogLowBoost = a.fogLowBoost;
    w.fogStart = a.fogStart;
    w.sunIntensity = a.sunIntensity;
    w.ambientIntensity = a.ambientIntensity;
    w.saturation = a.saturation;
    w.warmth = a.warmth;
    w.volumetricIntensity = a.volumetric;
    w.godRayIntensity = a.godRayIntensity;
    w.bloomIntensity = a.bloomIntensity;
    w.windStrength = a.windStrength;
    w.waveChop = a.waveChop;
    w.stormFront = a.stormFront;     // brick 30
    w.rainIntensity = a.rainIntensity; // brick 31
    return w;
}

void applyTo(AtmosphereParams& a, const data::WeatherForm& w) {
    a.cloudCoverage = w.cloudCoverage;
    a.cloudScale = w.cloudScale;
    a.cloudHeight = w.cloudHeight;
    a.cloudShadow = w.cloudShadowStrength;
    a.fogDensity = w.fogDensity;
    a.fogHeightFalloff = w.fogHeightFalloff;
    a.fogLowBoost = w.fogLowBoost;
    a.fogStart = w.fogStart;
    a.sunIntensity = w.sunIntensity;
    a.ambientIntensity = w.ambientIntensity;
    a.saturation = w.saturation;
    a.warmth = w.warmth;
    a.volumetric = w.volumetricIntensity;
    a.godRayIntensity = w.godRayIntensity;
    a.bloomIntensity = w.bloomIntensity;
    a.windStrength = w.windStrength;
    a.waveChop = w.waveChop;
    a.stormFront = w.stormFront;       // brick 30
    a.rainIntensity = w.rainIntensity; // brick 31
}

} // namespace

void WeatherController::init(const data::FormDatabase& forms) {
    weathers_ = data::resolveWeatherForms(forms);
}

void WeatherController::beginTransition(i32 index, const AtmosphereParams& current) {
    selected_ = index;
    if (selected_ >= 0) {
        from_ = capture(current);
        blend_ = 0.0f;
    }
}

void WeatherController::update(AtmosphereParams& atmos, f32 dt) {
    // Every parameter slides from the captured start state to the selected
    // weather over `duration_` seconds.
    if (blend_ >= 1.0f || selected_ < 0 ||
        selected_ >= static_cast<i32>(weathers_.size())) {
        return;
    }
    blend_ = glm::min(blend_ + dt / glm::max(duration_, 0.01f), 1.0f);
    const data::WeatherForm& to = weathers_[selected_];
    const f32 t = glm::smoothstep(0.0f, 1.0f, blend_);
    const auto lerp = [t](f32 a, f32 b) { return glm::mix(a, b, t); };
    data::WeatherForm blended;
    blended.cloudCoverage = lerp(from_.cloudCoverage, to.cloudCoverage);
    blended.cloudScale = lerp(from_.cloudScale, to.cloudScale);
    blended.cloudHeight = lerp(from_.cloudHeight, to.cloudHeight);
    blended.cloudShadowStrength = lerp(from_.cloudShadowStrength,
                                       to.cloudShadowStrength);
    blended.fogDensity = lerp(from_.fogDensity, to.fogDensity);
    blended.fogHeightFalloff = lerp(from_.fogHeightFalloff, to.fogHeightFalloff);
    blended.fogLowBoost = lerp(from_.fogLowBoost, to.fogLowBoost);
    blended.fogStart = lerp(from_.fogStart, to.fogStart);
    blended.sunIntensity = lerp(from_.sunIntensity, to.sunIntensity);
    blended.ambientIntensity = lerp(from_.ambientIntensity, to.ambientIntensity);
    blended.saturation = lerp(from_.saturation, to.saturation);
    blended.warmth = lerp(from_.warmth, to.warmth);
    blended.volumetricIntensity = lerp(from_.volumetricIntensity,
                                       to.volumetricIntensity);
    blended.godRayIntensity = lerp(from_.godRayIntensity, to.godRayIntensity);
    blended.bloomIntensity = lerp(from_.bloomIntensity, to.bloomIntensity);
    blended.windStrength = lerp(from_.windStrength, to.windStrength);
    blended.waveChop = lerp(from_.waveChop, to.waveChop);
    blended.stormFront = lerp(from_.stormFront, to.stormFront);     // brick 30
    blended.rainIntensity = lerp(from_.rainIntensity, to.rainIntensity); // 31
    applyTo(atmos, blended);
}

} // namespace game
