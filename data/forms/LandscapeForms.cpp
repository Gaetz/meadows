#include "data/forms/LandscapeForms.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"

namespace data {

namespace {
const core::Guid kLandscapeTuningGuid =
    *core::Guid::fromString("1a4d5c00-0000-4000-8000-000000000001");
// Tree builder: one tuning record per procedural tree type.
const core::Guid kLobeTreeTuningGuid =
    *core::Guid::fromString("1a4d5c00-0000-4000-8000-0000000000b1");
const core::Guid kColonizedTreeTuningGuid =
    *core::Guid::fromString("1a4d5c00-0000-4000-8000-0000000000b2");
} // namespace

void registerLandscapeFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<LandscapeTuningForm>();
    registry.registerFormType<LobeTreeTuningForm>();
    registry.registerFormType<ColonizedTreeTuningForm>();
    registry.registerFormType<WeatherForm>();
}

LandscapeTuningForm resolveLandscapeTuning(const FormDatabase& forms) {
    if (const LandscapeTuningForm* tuning =
            forms.find<LandscapeTuningForm>(kLandscapeTuningGuid)) {
        return *tuning;
    }
    return LandscapeTuningForm {};
}

LobeTreeTuningForm resolveLobeTreeTuning(const FormDatabase& forms) {
    if (const LobeTreeTuningForm* tuning =
            forms.find<LobeTreeTuningForm>(kLobeTreeTuningGuid)) {
        return *tuning;
    }
    return LobeTreeTuningForm {};
}

ColonizedTreeTuningForm
resolveColonizedTreeTuning(const FormDatabase& forms) {
    if (const ColonizedTreeTuningForm* tuning =
            forms.find<ColonizedTreeTuningForm>(kColonizedTreeTuningGuid)) {
        return *tuning;
    }
    return ColonizedTreeTuningForm {};
}

vector<WeatherForm> resolveWeatherForms(const FormDatabase& forms) {
    vector<WeatherForm> weathers;
    forEach<WeatherForm>(forms, [&](const WeatherForm& weather) {
        weathers.push_back(weather);
    });
    std::sort(weathers.begin(), weathers.end(),
              [](const WeatherForm& a, const WeatherForm& b) {
                  return a.sortOrder < b.sortOrder;
              });
    return weathers;
}

} // namespace data
