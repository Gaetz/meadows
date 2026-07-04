#include "game/scenes/LandscapeTuning.hpp"

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"

namespace game {

namespace {
const core::Guid kLandscapeTuningGuid =
    *core::Guid::fromString("1a4d5c00-0000-4000-8000-000000000001");
} // namespace

void registerLandscapeFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<LandscapeTuningForm>();
}

LandscapeTuningForm resolveLandscapeTuning(const data::FormDatabase& forms) {
    if (const LandscapeTuningForm* tuning =
            forms.find<LandscapeTuningForm>(kLandscapeTuningGuid)) {
        return *tuning;
    }
    return LandscapeTuningForm {};
}

} // namespace game
