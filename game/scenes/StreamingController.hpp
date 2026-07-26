#pragma once

#include <unordered_map>
#include <unordered_set>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp"          // ecs::World, flecs::query
#include "engine/physics/Physics.hpp"    // phys::BodyId
#include "world/scene/Components.hpp"    // world::Transform, RefId, MeshRender

namespace render {
struct TerrainParams;
}
namespace data {
class FormDatabase;
}
namespace phys {
class PhysicsWorld;
}
namespace world {
class TerrainNavigator;
}

namespace render {
class MeshCache;
}

namespace game {

// The scene systems the cell-streaming fixups touch, bundled so the streaming
// bookkeeping (ground snap, static-collider cook, nav obstacles) is decoupled
// from LandscapeScene. The scene rebuilds it each frame from its
// own members — cheap: references plus a couple of per-frame scalars. This is
// the streaming↔scene contract, mirroring EditorContext.
struct StreamingContext {
    ecs::World& world;                          // fixup queries
    data::FormDatabase& forms;                  // base/ref/cell + `collides`
    const render::TerrainParams& terrainParams; // ground snap (height)
    phys::PhysicsWorld* physics;                // static-mesh bodies
    render::MeshCache* meshCache;               // CpuMesh (cook + nav AABB)
    world::TerrainNavigator* navigator;          // blocking boxes
    Vec3 focus;                 // player/camera position — cook nearest first
    bool fastCook;              // travel fade holds a black screen -> uncap cook
    bool editorOwnsTransforms;  // Edit mode: skip the ground snap
};

// Cell-streaming bookkeeping extracted from LandscapeScene: the
// static-collider set, the negative-verdict cache, and the three fixups that
// run when the cell ring changes (snap, nav) or every frame (colliders). NPC
// (re)building stays in the scene (NpcDirector territory) — the scene still
// interleaves it between snap and nav to preserve order.
class StreamingController {
public:
    // Build the cached collider query from the world (once, after onEnter's
    // world (re)creation — a query is an allocation, never per frame).
    void init(ecs::World& world);

    // On a cell-ring change (idempotent, safe to re-run per loaded cell).
    void snapCellEntities(const StreamingContext& ctx);
    void refreshNavObstacles(const StreamingContext& ctx);

    // Per frame: static bodies follow spawns + mesh residency (budgeted cook).
    void updateStaticColliders(const StreamingContext& ctx);

    // onExit teardown: remove every owned static body (before the physics
    // world is destroyed) and drop the caches, so a re-enter starts clean.
    void reset(phys::PhysicsWorld* physics);

private:
    flecs::query<const world::Transform, const world::RefId,
                 const world::MeshRender>
        colliderQuery;
    // Cell references that own a Jolt static body, so it can be removed when the
    // entity's cell unloads. Keyed by flecs entity id. `nonCollidable` caches
    // the negative reflection verdict so `collides` is not read per frame
    // (snapCellEntities clears it on any ring change).
    std::unordered_map<u64, phys::BodyId> staticColliders;
    std::unordered_set<u64> nonCollidable;
};

} // namespace game
