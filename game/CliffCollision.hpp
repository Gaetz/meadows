#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/physics/Physics.hpp"
#include "engine/render/landscape/CliffSystem.hpp"

namespace game {

// Collision for the baked cliff BLOCKS (docs/CLIFFS.md étage 2): one
// oriented static box per planned block near the player — the ledge
// tops are STANDABLE terrain, so the boxes must match the visuals.
// The plan is the same deterministic function the renderer meshes
// (render::planCliffBlocks), re-run per chunk on this ring — the
// TerrainCollision/VegetationCollision streaming pattern (3x3 ring,
// one chunk cooked per update, hysteresis eviction). Watches the
// terrain base pointer: a republish drops every body and re-cooks.
class CliffCollision {
public:
    CliffCollision(phys::PhysicsWorld& physics,
                   const render::TerrainParams* params);
    ~CliffCollision();

    CliffCollision(const CliffCollision&) = delete;
    CliffCollision& operator=(const CliffCollision&) = delete;

    void update(const Vec3& focus);

    u32 bodyCount() const { return bodies; }

private:
    void cookChunk(i32 cx, i32 cz);
    void clearAll();

    phys::PhysicsWorld& physics;
    const render::TerrainParams* params;
    const void* baseKey { nullptr };
    std::unordered_map<u64, vector<phys::BodyId>> chunks;
    u32 bodies { 0 };
};

} // namespace game
