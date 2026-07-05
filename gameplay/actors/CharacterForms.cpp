#include "gameplay/actors/CharacterForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

void registerCharacterFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<AppearanceForm>();
}

} // namespace gameplay
