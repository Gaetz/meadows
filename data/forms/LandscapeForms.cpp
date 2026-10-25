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
    *core::Guid::fromString("7cdab9a1-6af4-4c89-a161-bc45c3ae8ea3");
const core::Guid kColonizedTreeTuningGuid =
    *core::Guid::fromString("a344e05d-c66b-4c93-81dd-b535a0072dd0");
const core::Guid kRcTuningGuid =
    *core::Guid::fromString("1a4d5c00-0000-4000-8000-000000000009");
const core::Guid kTerrainGenTuningGuid =
    *core::Guid::fromString("1a4d5c00-0000-4000-8000-00000000000a");
} // namespace

const core::Guid& landscapeTuningGuid() { return kLandscapeTuningGuid; }
const core::Guid& lobeTreeTuningGuid() { return kLobeTreeTuningGuid; }
const core::Guid& colonizedTreeTuningGuid() {
    return kColonizedTreeTuningGuid;
}
const core::Guid& rcTuningGuid() { return kRcTuningGuid; }
const core::Guid& terrainGenTuningGuid() { return kTerrainGenTuningGuid; }

void registerLandscapeFormTypes(FormTypeRegistry& registry) {
    registry.registerFormType<LandscapeTuningForm>();
    registry.registerFormType<LobeTreeTuningForm>();
    registry.registerFormType<ColonizedTreeTuningForm>();
    registry.registerFormType<RcTuningForm>();
    registry.registerFormType<TerrainGenTuningForm>();
    registry.registerFormType<ZoneArchetypeForm>();
    registry.registerFormType<WeatherForm>();
    registry.registerFormType<RiverSourceFxForm>();
}

LandscapeTuningForm resolveLandscapeTuning(const FormDatabase& forms) {
    return resolveSingleton<LandscapeTuningForm>(forms, kLandscapeTuningGuid);
}

LobeTreeTuningForm resolveLobeTreeTuning(const FormDatabase& forms) {
    return resolveSingleton<LobeTreeTuningForm>(forms, kLobeTreeTuningGuid);
}

ColonizedTreeTuningForm
resolveColonizedTreeTuning(const FormDatabase& forms) {
    return resolveSingleton<ColonizedTreeTuningForm>(forms, kColonizedTreeTuningGuid);
}

RcTuningForm resolveRcTuning(const FormDatabase& forms) {
    return resolveSingleton<RcTuningForm>(forms, kRcTuningGuid);
}

TerrainGenTuningForm resolveTerrainGenTuning(const FormDatabase& forms) {
    return resolveSingleton<TerrainGenTuningForm>(forms, kTerrainGenTuningGuid);
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
