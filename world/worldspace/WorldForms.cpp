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
    registry.registerFormType<TerrainRegionForm>();
    registry.registerFormType<WaterMaterialForm>();
    registry.registerFormType<WaterBodyForm>();
    registry.registerFormType<RiverForm>();
    registry.registerFormType<RiverPointForm>();
    registry.registerFormType<BiomeForm>();
    registry.registerFormType<BiomeVegetationForm>();
    registry.registerFormType<BiomeMapForm>();
}

core::Guid cellGuidFor(const core::Guid& worldspace, i32 gx, i32 gy) {
    // The grid square as a synthetic guid (coords in hi, a fixed tag in
    // lo so cell coords can never collide with another derived family),
    // mixed with the worldspace identity through the SAME deterministic
    // combine the prefab-child contract uses (H8) — well-formed v4 out.
    const core::Guid coords {
        (static_cast<u64>(static_cast<u32>(gx)) << 32) |
            static_cast<u64>(static_cast<u32>(gy)),
        0x63656C6C67726964ull // "cellgrid"
    };
    return core::Guid::combine(worldspace, coords);
}

} // namespace world
