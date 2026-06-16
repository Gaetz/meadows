#include "gameplay/stats/StatsTuning.hpp"

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/stats/Afflictions.hpp" // registerAfflictionFormTypes
#include "gameplay/stats/Drugs.hpp"       // registerDrugFormTypes

namespace gameplay {

namespace {
// Canonical guid a mod patches to retune the stats system.
const core::Guid kStatsTuningGuid =
    *core::Guid::fromString("57a70000-0000-4000-8000-000000000001");
} // namespace

void registerStatsFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<StatsTuningForm>();
    registerAfflictionFormTypes(registry); // diseases / psychoses (N3)
    registerDrugFormTypes(registry);       // drugs (N4)
}

StatsTuningForm resolveStatsTuning(const data::FormDatabase& forms) {
    if (const StatsTuningForm* tuning =
            forms.find<StatsTuningForm>(kStatsTuningGuid)) {
        return *tuning;
    }
    return StatsTuningForm {};
}

} // namespace gameplay
