#include "gameplay/interaction/FurnitureForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

void registerFurnitureFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<FurnitureForm>();
    registry.registerFormType<FurniturePointForm>();
}

FurnitureInteraction furnitureInteraction(const FurnitureForm& furniture) {
    const str& key = furniture.interaction.empty() ? furniture.category
                                                   : furniture.interaction;
    return reflect::parseEnum<FurnitureInteraction>(kFurnitureInteractions,
                                                    key)
        .value_or(FurnitureInteraction::Use);
}

f32 furnitureRestHours(const FurnitureForm& furniture) {
    if (furniture.restHours > 0.0f) {
        return furniture.restHours;
    }
    return furniture.category == "bed" ? 8.0f : 1.0f;
}

} // namespace gameplay
