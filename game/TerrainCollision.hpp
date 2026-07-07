#pragma once

#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Jobs.hpp"
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
// v2 (anti-stutter): the 4096-sample noise pass — 95 % of a tile's cost —
// runs on a WORKER; the main thread only lands the Jolt body (cheap).
// Requests are heading-biased: the tile you are flying toward is queued
// one tile early, so it normally lands before you cross. One guarantee
// stays synchronous: if the tile UNDER the focus is missing (teleport,
// travel), it is cooked on the spot — the capsule never stands on
// nothing. Without a JobSystem (headless tests) everything falls back to
// one synchronous cook per update, nearest first.
class TerrainCollision {
public:
    static constexpr u32 kSamples = 64;   // per side (Jolt-friendly 2^n)
    static constexpr f32 kSpacing = 1.0f; // meters between samples

    TerrainCollision(phys::PhysicsWorld& physics,
                     const render::TerrainParams& params,
                     core::JobSystem* jobs = nullptr);
    ~TerrainCollision();

    TerrainCollision(const TerrainCollision&) = delete;
    TerrainCollision& operator=(const TerrainCollision&) = delete;

    // Converges the 3x3 tile ring around `focus` (+ one heading-predicted
    // tile); evicts tiles more than one ring beyond it (hysteresis — a
    // tile-border stroll never churns).
    void update(const Vec3& focus);

    u32 tileCount() const { return static_cast<u32>(tiles.size()); }

private:
    static constexpr f32 kTileEdge = (kSamples - 1) * kSpacing;

    struct SampledTile {
        u64 key { 0 };
        Vec3 origin {};
        vector<f32> samples;
    };

    void addTile(u64 key, const Vec3& origin, const f32* samples);
    void cookSync(i32 tx, i32 tz);
    // Queues the worker sample for a missing tile (or cooks it directly
    // in the synchronous fallback when `budget` allows). Returns whether
    // the fallback budget was consumed.
    bool request(i32 tx, i32 tz, bool& budget);

    phys::PhysicsWorld& physics;
    render::TerrainParams params;
    core::JobSystem* jobs { nullptr };
    // Results mailbox (Phase-5 pattern): workers push, the frame thread
    // drains. shared_ptr so in-flight jobs outlive a teardown harmlessly.
    std::shared_ptr<core::ConcurrentQueue<SampledTile>> built;
    std::unordered_map<u64, phys::BodyId> tiles; // key = packed tile coord
    std::unordered_set<u64> pending;             // queued on a worker
    Vec3 lastFocus {};
    bool hasLastFocus { false };
};

} // namespace game
