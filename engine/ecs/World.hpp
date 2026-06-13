#pragma once

#include <unordered_map>

#include <flecs.h>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

// The ECS layer (Phase 2). flecs is the storage/query engine, used directly in
// systems (it is deliberately NOT hidden behind an RHI-style interface — only
// the renderer earns that, §2.1). flecs is confined to the runtime: the data
// model (Forms, resolver, save-as-patch) stays flecs-free, and our reflection
// remains the keystone (§2.3) for serialization, patching and saves.
//
// An entity is the runtime instance of a placed Reference (§2.2). flecs ids are
// OPAQUE runtime tokens — they encode a generation (and, for relationship
// pairs, the relation + target) in their high bits. NEVER persist them, index
// on them, or derive meaning from them: persistence keys on the GUIDs carried
// by components (e.g. RefId), and a stale handle is detectable via
// Entity::is_alive().

namespace ecs {

using Entity = flecs::entity;

// Relation tag: a placed reference belongs to a cell. Posed at spawn as
// `entity.add<InCell>(cellEntity)`. This is a pure runtime projection of the
// resolved CellForm, jettisoned at unload — never serialized. The moddable /
// persisted truth of "what is in a cell" is the ReferenceForm.cell field (§5).
struct InCell {};

// Thin owner of the flecs world plus the bridge that ties components into our
// reflection. Systems reach the flecs API through handle().
class World {
public:
    World() = default;

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = default;
    World& operator=(World&&) = default;

    flecs::world& handle() noexcept { return world; }
    const flecs::world& handle() const noexcept { return world; }

    Entity create() { return world.entity(); }

    // Registers a component in BOTH systems at once: flecs storage AND our
    // reflected-component registry, so Phase 5 saves can serialize component
    // state through the same reflect::Value pipeline as Forms (§2.8). The
    // single registration point that keeps the two from diverging.
    template<typename T>
    Entity registerComponent() {
        const flecs::component<T> component = world.component<T>();
        reflectedComponents.emplace(component.id(), &T::staticTypeInfo());
        return component;
    }

    // Reflection type info for a flecs component id, or nullptr if it was not
    // registered through registerComponent (e.g. an internal flecs component).
    // Drives generic component serialization (Phase 5).
    const reflect::TypeInfo* reflectedComponent(flecs::id_t componentId) const;

private:
    flecs::world world;
    std::unordered_map<flecs::id_t, const reflect::TypeInfo*> reflectedComponents;
};

} // namespace ecs
