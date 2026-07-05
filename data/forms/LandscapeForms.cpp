#include "data/forms/LandscapeForms.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"

namespace data {

namespace {
const core::Guid kLandscapeTuningGuid =
    *core::Guid::fromString("1a4d5c00-0000-4000-8000-000000000001");
} // namespace

void registerLandscapeFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<LandscapeTuningForm>();
    registry.registerFormType<WeatherForm>();
}

LandscapeTuningForm resolveLandscapeTuning(const FormDatabase& forms) {
    if (const LandscapeTuningForm* tuning =
            forms.find<LandscapeTuningForm>(kLandscapeTuningGuid)) {
        return *tuning;
    }
    return LandscapeTuningForm {};
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
