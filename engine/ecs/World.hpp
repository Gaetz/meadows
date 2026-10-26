#pragma once

#include <flecs.h>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

// The ECS layer (docs/PHASE-2.md). flecs is the storage/query engine, used directly in
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

// The reflection link of a component registered through
// registerReflectedComponent. flecs components are entities, and this link
// rides ON the component entity: any entity handle in the world can then
// enumerate its reflected components (forEachReflectedComponent) without
// reaching the World object that registered them — the save layer holds
// only entity handles. Moving a World moves the link with the flecs world.
struct ReflectedComponent {
    const reflect::TypeInfo* type { nullptr };
};

// Registers T in BOTH systems at once: flecs storage AND our reflection
// bridge, so saves can serialize component state through the same
// reflect::Value pipeline as Forms (§2.8). Idempotent — a component that
// flecs already knows (an earlier set<T>, another registration) just gets
// linked. The single registration point that keeps the two from diverging;
// World::registerComponent is its member spelling.
template<typename T>
Entity registerReflectedComponent(const flecs::world& world) {
    const Entity component = world.component<T>();
    if (!component.has<ReflectedComponent>()) {
        component.set<ReflectedComponent>({ &T::staticTypeInfo() });
    }
    return component;
}

// Reflection type info for a component id, or nullptr when the id is a
// pair / flagged id, or a component never registered through
// registerReflectedComponent (an internal flecs component, a tag, a
// runtime-only component).
const reflect::TypeInfo* reflectedComponentOf(flecs::id id);

// Visits every REFLECTED component the entity carries, in flecs type order
// (not a stable order — callers that need one sort by their own list):
// fn(const reflect::TypeInfo&, void* storage). Drives generic component
// serialization for saves. Non-reflected components are skipped.
template<typename Fn>
void forEachReflectedComponent(Entity entity, Fn&& fn) {
    entity.each([&](flecs::id id) {
        if (const reflect::TypeInfo* type = reflectedComponentOf(id)) {
            if (void* storage = entity.get_mut(id.raw_id())) {
                fn(*type, storage);
            }
        }
    });
}

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

    // registerReflectedComponent on this world — see there.
    template<typename T>
    Entity registerComponent() {
        return registerReflectedComponent<T>(world);
    }

    // reflectedComponentOf for a raw component id of this world.
    const reflect::TypeInfo* reflectedComponent(flecs::id_t componentId) const;

private:
    flecs::world world;
};

} // namespace ecs
