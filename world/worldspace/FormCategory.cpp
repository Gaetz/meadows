#include "world/worldspace/FormCategory.hpp"

#include "data/forms/CoreForms.hpp"

namespace world {

void registerCoreCategories(FormCategoryRegistry& registry) {
    registry.set<data::WeaponForm>(FormCategory::Item);
    registry.set<data::ActorForm>(FormCategory::Actor);
}

} // namespace world
