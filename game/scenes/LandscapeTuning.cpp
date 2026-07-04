#include "game/scenes/LandscapeTuning.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"

namespace game {

namespace {
const core::Guid kLandscapeTuningGuid =
    *core::Guid::fromString("1a4d5c00-0000-4000-8000-000000000001");
} // namespace

void registerLandscapeFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<LandscapeTuningForm>();
    registry.registerFormType<WeatherForm>();
}

LandscapeTuningForm resolveLandscapeTuning(const data::FormDatabase& forms) {
    if (const LandscapeTuningForm* tuning =
            forms.find<LandscapeTuningForm>(kLandscapeTuningGuid)) {
        return *tuning;
    }
    return LandscapeTuningForm {};
}

vector<WeatherForm> resolveWeatherForms(const data::FormDatabase& forms) {
    vector<WeatherForm> weathers;
    for (u32 i = 1; i <= forms.count(); ++i) {
        const data::FormHandle handle { i };
        if (!forms.typeOf(handle)->isA(WeatherForm::staticTypeInfo().id)) {
            continue;
        }
        weathers.push_back(
            *static_cast<const WeatherForm*>(forms.get(handle)));
    }
    std::sort(weathers.begin(), weathers.end(),
              [](const WeatherForm& a, const WeatherForm& b) {
                  return a.sortOrder < b.sortOrder;
              });
    return weathers;
}

} // namespace game
