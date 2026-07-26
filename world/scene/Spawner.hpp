#pragma once

#include <functional>
#include <unordered_map>

#include "data/forms/FormDatabase.hpp"
#include "engine/ecs/World.hpp"
#include "world/scene/Components.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldForms.hpp"

// The reference→entity spawner (§2.7): a resolved base Form + a ReferenceForm
// become an ECS entity, keyed on the base form's category. The universal
// components (Transform, SpriteRender, RefId, InCell) are wired here, applying
// values **through reflection** where the base type is polymorphic; the
// per-category hook adds category-specific components (markers, the
// actor hook's AbilitySystem, etc.).

namespace world {

struct SpawnContext {
    ecs::World& world;
    const data::FormDatabase& forms;
    const FormCategoryRegistry& categories;
    // Optional per-reference veto: the pending save layer
    // suppresses references it knows are disabled. Consulted by the
    // prefab expansion (derived children bypass the cell loop) — the
    // CellLoader applies it to top-level references itself.
    std::function<bool(const core::Guid& referenceId)> filter {};
};

// Category-specific wiring, called after the universal components are set.
using SpawnFn = void (*)(SpawnContext&, ecs::Entity entity,
                         const data::Form& base,
                         const reflect::TypeInfo& baseType);

class Spawner {
public:
    void registerCategory(FormCategory category, SpawnFn fn) {
        byCategory.insert_or_assign(category, fn);
    }

    // Spawns one reference under cellEntity (pass an invalid entity for none).
    // Returns an invalid entity (and logs) if the base form is unresolvable or
    // its category has no registered spawner. Does not check `enabled` — that
    // is the cell-loader's call (brick e).
    ecs::Entity spawn(SpawnContext& ctx, const ReferenceForm& reference,
                      ecs::Entity cellEntity) const;

private:
    std::unordered_map<FormCategory, SpawnFn> byCategory;
};

// Installs the default spawners for the core categories (Static, Item, Actor).
void registerCoreSpawners(Spawner& spawner);

} // namespace world
