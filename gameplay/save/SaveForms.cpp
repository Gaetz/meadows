#include "gameplay/save/SaveForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

void registerSaveFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<SavedStatsForm>();
    registry.registerFormType<SavedEffectForm>();
    registry.registerFormType<SavedItemForm>();
    registry.registerFormType<SavedInjuryForm>();
    registry.registerFormType<SavedAbilityForm>(); // FOLLOWERS É6
    registry.registerFormType<SavedSkillForm>();   // skills-by-use
    registry.registerFormType<WorldStateForm>();
}

} // namespace gameplay
