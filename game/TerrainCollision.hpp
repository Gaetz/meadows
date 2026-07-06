#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/physics/Physics.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"

namespace game {

// Terrain collision (chantier 1, B4): keeps Jolt height-field tiles alive
// around a focus point (the player), sampled straight from the SAME
// deterministic terrain function the renderer uses — collision is
// independent of the render LOD/skirts by construction. Tiles live on
// their own grid: 64x64 samples at 1 m, i.e. 63 m per tile, edge samples
// shared with the next tile (seamless).
//
// v1 is synchronous: a ring of tiles is (re)built when the focus crosses a
// tile border (~1 ms/tile). Move it to the worker/queue pattern if it ever
// shows in the frame — do not pre-build that machinery.
class TerrainCollision {
public:
    static constexpr u32 kSamples = 64;   // per side (Jolt-friendly 2^n)
    static constexpr f32 kSpacing = 1.0f; // meters between samples

    TerrainCollision(phys::PhysicsWorld& physics,
                     const render::TerrainParams& params);
    ~TerrainCollision();

    TerrainCollision(const TerrainCollision&) = delete;
    TerrainCollision& operator=(const TerrainCollision&) = delete;

    // Ensures the 3x3 tile ring around `focus` exists; evicts tiles more
    // than one ring beyond it (hysteresis — border strolls don't churn).
    void update(const Vec3& focus);

    u32 tileCount() const { return static_cast<u32>(tiles.size()); }

private:
    static constexpr f32 kTileEdge = (kSamples - 1) * kSpacing;

    phys::PhysicsWorld& physics;
    render::TerrainParams params;
    std::unordered_map<u64, phys::BodyId> tiles; // key = packed tile coord
};

} // namespace game
