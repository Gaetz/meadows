#include "world/worldspace/WorldForms.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace world {

void registerWorldFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<WorldspaceForm>();
    registry.registerFormType<CellForm>();
    registry.registerFormType<ReferenceForm>();
    registry.registerFormType<PrefabForm>();
    registry.registerFormType<MarkerForm>();
    registry.registerFormType<TriggerForm>();
    registry.registerFormType<DoorForm>();
    registry.registerFormType<TerrainPatchForm>();
}

} // namespace world
