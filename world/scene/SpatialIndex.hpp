#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp"

// Chantier P0 B1 — the shared spatial index (the infrastructure promised
// by the RC-GI chantier, blessed by the dev: created when a gameplay
// consumer bites). A uniform hash grid over the XZ plane (actors live on
// a heightfield; 4 m cells), rebuilt from the live world each frame and
// queried by radius or cone. A rebuild SNAPSHOTS entity + position, so
// consumers that dispatch callbacks (triggers) keep their iteration
// stable even when handlers spawn or despawn entities mid-tick.
//
// v1 indexes ACTORS (Transform + ActorMarker) — the consumers are the
// trigger sweep, perception (B2) and combat AI (B3). Distances are full
// 3D; only the BUCKETING is planar. Upgrade path: more archetypes, or an
// octree, the day a profile asks for it.

namespace world {

class SpatialIndex {
public:
    struct Entry {
        ecs::Entity entity;
        Vec3 position; // snapshot at rebuild time
    };

    explicit SpatialIndex(f32 cellSize = 4.0f) : cellSize { cellSize } {}

    // Re-snapshots every actor. Call once per frame before the queries.
    void rebuild(ecs::World& world);

    // Every snapshotted actor within `radius` (3D distance) of `center`.
    // Appends to `out` (callers reuse a scratch vector).
    void queryRadius(const Vec3& center, f32 radius,
                     vector<Entry>& out) const;

    // Every actor within `range` of `apex` AND inside the cone around
    // `direction` (normalized) of half-angle cosine `cosHalfAngle`.
    // The apex itself (distance ~0) counts as inside.
    void queryCone(const Vec3& apex, const Vec3& direction, f32 range,
                   f32 cosHalfAngle, vector<Entry>& out) const;

    u32 size() const { return count; }

private:
    // Cell coordinates packed into one key (i32 range is plenty).
    static u64 key(i32 x, i32 z) {
        return (static_cast<u64>(static_cast<u32>(x)) << 32) |
               static_cast<u64>(static_cast<u32>(z));
    }
    i32 cellOf(f32 v) const {
        return static_cast<i32>(std::floor(v / cellSize));
    }

    f32 cellSize;
    u32 count { 0 };
    std::unordered_map<u64, vector<Entry>> cells;
};

} // namespace world
