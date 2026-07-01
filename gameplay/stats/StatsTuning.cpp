#include "gameplay/stats/StatsTuning.hpp"

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/stats/Injuries.hpp" // registerInjuryTags
#include "gameplay/stats/Survival.hpp" // registerSurvivalTags

namespace gameplay {

namespace {
const core::Guid kStatsTuningGuid =
    *core::Guid::fromString("57a70000-0000-4000-8000-000000000001");
} // namespace

void registerStatsFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<StatsTuningForm>();
    // AfflictionForm and DrugForm are now EffectForms — no separate registration.
}

void registerStatsRuntimeTags(GameplayTagRegistry& tags) {
    registerInjuryTags(tags);
    registerSurvivalTags(tags);
}

StatsTuningForm resolveStatsTuning(const data::FormDatabase& forms) {
    if (const StatsTuningForm* tuning =
            forms.find<StatsTuningForm>(kStatsTuningGuid)) {
        return *tuning;
    }
    return StatsTuningForm {};
}

} // namespace gameplay
