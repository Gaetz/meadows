#pragma once

// Compatibility shim: the landscape/weather Forms moved
// to meadows-data (data/forms/LandscapeForms.hpp) so TOOLS — the cooker,
// the Game DB editor — can register them; an exe-local Form type is
// invisible to every consumer but the game. Reflected names are
// unqualified, so on-disk TOML is untouched. Existing game code keeps its
// unqualified spellings through these aliases.

#include "data/forms/LandscapeForms.hpp"

namespace game {

using data::LandscapeTuningForm;
using data::WeatherForm;
using data::registerLandscapeFormTypes;
using data::resolveLandscapeTuning;
using data::resolveWeatherForms;

} // namespace game
