#include "gameplay/interaction/FurnitureForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

void registerFurnitureFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<FurnitureForm>();
    registry.registerFormType<FurniturePointForm>();
}

} // namespace gameplay
