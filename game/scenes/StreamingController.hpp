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

// Which world the scene is in. Decided in ONE place (the scene's
// decideWorldKind, fed by the world entry / travel path); every per-world
// gate reads the kind or its policy instead of re-deriving it from the
// streamer, the sandbox params or the interior flag.
enum class WorldKind : u8 {
    AuthoredCells, // the authored exterior over the legacy terrain
    SandboxMap,    // a bounded generated map, its tiles streamed by the baker
    Interior,      // an interior worldspace: no terrain, sky, water or weather
};

// What a world kind affords — one flat table (worldPolicy), read by the
// gates of the frame instead of the kind's ingredients.
struct WorldPolicy {
    // The height field is the ground: footstep material, terrain and
    // vegetation collision, ground-rise throws, the swimmer's water.
    bool snapsToTerrain;
    // The tile streamer runs and its ring completion gates the veil.
    bool streamsBakes;
    // Sea, lakes and rivers exist: floaters, swimming, river dressing.
    bool hasWaterBodies;
    // Wind and rain reach the props (torch snuff).
    bool hasWeather;
    // The spirit fields (fire, flame jet, igniters, water jets) act on
    // the ground and the actors.
    bool allowsSpirits;
    // A map rim exists (the ocean's turn-back current past it).
    bool hasBoundedMap;
};

constexpr WorldPolicy worldPolicy(WorldKind kind) {
    switch (kind) {
    case WorldKind::AuthoredCells:
        return WorldPolicy { .snapsToTerrain = true,
                             .streamsBakes = false,
                             .hasWaterBodies = true,
                             .hasWeather = true,
                             .allowsSpirits = true,
                             .hasBoundedMap = false };
    case WorldKind::SandboxMap:
        return WorldPolicy { .snapsToTerrain = true,
                             .streamsBakes = true,
                             .hasWaterBodies = true,
                             .hasWeather = true,
                             .allowsSpirits = true,
                             .hasBoundedMap = true };
    case WorldKind::Interior:
        break;
    }
    return WorldPolicy { .snapsToTerrain = false,
                         .streamsBakes = false,
                         .hasWaterBodies = false,
                         .hasWeather = false,
                         .allowsSpirits = false,
                         .hasBoundedMap = false };
}

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
    WorldKind worldKind;        // the active world, decided by the scene
    WorldPolicy policy;         // what it affords (worldPolicy(worldKind))
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

    // Hands an entity's static body over to another owner (the earth
    // spirit seizing a rock): erased here and never re-cooked; 0 if none.
    phys::BodyId takeStaticCollider(u64 entityId);

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
    std::unordered_set<u64> seized; // bodies handed over (never re-cooked)
};

} // namespace game
