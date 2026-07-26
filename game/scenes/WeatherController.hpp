#pragma once

#include "data/forms/LandscapeForms.hpp" // WeatherForm, resolveWeatherForms
#include "engine/core/Defines.hpp"
#include "engine/render/AtmosphereParams.hpp"

namespace data {
class FormDatabase;
}

namespace game {

// Owns the weather crossfade (extracted from LandscapeScene): a set
// of precreated WeatherForm states (from landscape.toml) and a smooth
// transition that writes the interpolated result into the scene's
// AtmosphereParams over `duration` seconds. The scene keeps the manual sliders
// and the ImGui "Weather" panel, driving this through beginTransition()/
// update(); render() reads only render::AtmosphereParams.
class WeatherController {
public:
    void init(const data::FormDatabase& forms); // resolve the WeatherForm states
    // Advances the active crossfade.
    void update(render::AtmosphereParams& atmos, f32 dt);

    // Panel support (the ImGui combo/slider stay in the scene).
    const vector<data::WeatherForm>& states() const { return weathers_; }
    i32 selected() const { return selected_; }
    bool transitioning() const { return blend_ < 1.0f && selected_ >= 0; }
    f32 blend() const { return blend_; }
    f32& duration() { return duration_; }

    // Start fading to `index` (-1 = manual), departing from whatever is on
    // screen right now so a mid-fade switch stays continuous.
    void beginTransition(i32 index, const render::AtmosphereParams& current);

private:
    vector<data::WeatherForm> weathers_;
    i32 selected_ { -1 };    // index into weathers_, -1 = manual
    data::WeatherForm from_; // captured start state of the active transition
    f32 blend_ { 1.0f };     // 1 = arrived
    f32 duration_ { 30.0f };
};

} // namespace game
