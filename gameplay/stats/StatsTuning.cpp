#include "gameplay/stats/StatsTuning.hpp"

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

namespace {
// Canonical guid a mod patches to retune the stats system.
const core::Guid kStatsTuningGuid =
    *core::Guid::fromString("57a70000-0000-4000-8000-000000000001");
} // namespace

void registerStatsFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<StatsTuningForm>();
}

StatsTuningForm resolveStatsTuning(const data::FormDatabase& forms) {
    if (const StatsTuningForm* tuning =
            forms.find<StatsTuningForm>(kStatsTuningGuid)) {
        return *tuning;
    }
    return StatsTuningForm {};
}

} // namespace gameplay
