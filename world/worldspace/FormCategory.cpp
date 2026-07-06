#include "world/worldspace/FormCategory.hpp"

#include "data/forms/CoreForms.hpp"
#include "data/forms/VisualForms.hpp"
#include "gameplay/interaction/FurnitureForms.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

void registerCoreCategories(FormCategoryRegistry& registry) {
    registry.set<data::WeaponForm>(FormCategory::Item);
    registry.set<data::ActorForm>(FormCategory::Actor);
    registry.set<data::StaticForm>(FormCategory::Static);
    registry.set<data::LightForm>(FormCategory::Light);
    registry.set<MarkerForm>(FormCategory::Marker);
    registry.set<TriggerForm>(FormCategory::Trigger);
    registry.set<DoorForm>(FormCategory::Door); // chantier 2 B7
    registry.set<PrefabForm>(FormCategory::Prefab);
    registry.set<gameplay::FurnitureForm>(FormCategory::Furniture);
}

} // namespace world
