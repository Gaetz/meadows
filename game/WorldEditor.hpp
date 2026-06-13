#pragma once

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp"
#include "world/scene/Spawner.hpp"
#include "world/worldspace/FormCategory.hpp"

namespace game {

// In-game dev tool (ImGui): add, select, move and delete objects in the active
// cell. Edits operate on instance state — the entity's Transform and spawned
// references — never on Forms (§2.2). They are NOT yet persisted to plugins:
// exporting live edits as a §5 patch layer is the §5.1 / Phase 9 editor concern,
// so reloading the world (e.g. the mod toggle) discards them.
//
// Lives in the reusable meadows-runtime lib, so a future editor can build on it.
class WorldEditor {
public:
    WorldEditor(ecs::World& world, const data::FormDatabase& forms,
                const world::FormCategoryRegistry& categories,
                const world::Spawner& spawner)
        : world { world }, forms { forms }, categories { categories },
          spawner { spawner } {}

    // The cell that "Add" places new objects into; refresh after each (re)load.
    void setActiveCell(ecs::Entity cellEntity, const core::Guid& cellId) {
        activeCell = cellEntity;
        activeCellId = cellId;
    }

    void drawUi();

private:
    ecs::World& world;
    const data::FormDatabase& forms;
    const world::FormCategoryRegistry& categories;
    const world::Spawner& spawner;

    ecs::Entity activeCell {};
    core::Guid activeCellId {};
    ecs::Entity selected {};
    int paletteIndex { 0 };
    float placePosition[2] { 0.0f, 0.0f };
};

} // namespace game
