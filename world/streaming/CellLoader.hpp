#pragma once

#include <functional>
#include <unordered_map>

#include "data/forms/FormDatabase.hpp"
#include "engine/ecs/World.hpp"
#include "world/scene/Spawner.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldModel.hpp"

namespace world {

// Turns a cell's resolved references into live entities and back. Phase 2:
// eager (loadAll at startup). The interface is the shape Phase 8's async
// streaming will implement (load/unload per cell, persistence on unload).
//
// Each loaded cell gets a flecs cell-entity; its references are spawned with an
// (InCell, cellEntity) relation, so unloadCell is a single delete_with. The
// cell-entity is an ephemeral runtime projection (§ docs/PHASE-2.md), never
// serialized.
class CellLoader {
public:
    CellLoader(ecs::World& world, const data::FormDatabase& forms,
               const WorldModel& model, const Spawner& spawner,
               const FormCategoryRegistry& categories)
        : world { world }, forms { forms }, model { model }, spawner { spawner },
          categories { categories } {}

    // Spawns the cell's enabled references (idempotent: returns the existing
    // cell-entity if already loaded).
    ecs::Entity loadCell(data::FormHandle cell);
    void unloadCell(data::FormHandle cell);

    void loadAll();   // every cell in the world model
    void unloadAll();

    // The cell-entity for a loaded cell, or an invalid entity if not loaded.
    ecs::Entity cellEntity(data::FormHandle cell) const;

    // Persistence seams (chantier 5). `beforeUnload` fires at the top of
    // unloadCell, while the cell's entities are still alive — the save
    // layer captures their deltas there. `spawnFilter` (when set) can veto
    // one reference's spawn by guid — the pending layer suppresses
    // references it knows are disabled (picked-up items) without touching
    // the resolved database.
    std::function<void(data::FormHandle cell, ecs::Entity cellEntity)>
        beforeUnload;
    std::function<bool(const core::Guid& referenceId)> spawnFilter;

private:
    ecs::World& world;
    const data::FormDatabase& forms;
    const WorldModel& model;
    const Spawner& spawner;
    const FormCategoryRegistry& categories;
    std::unordered_map<u32, ecs::Entity> loaded; // cell handle value -> cell-entity
};

} // namespace world
