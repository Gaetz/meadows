#pragma once

#include <unordered_map>
#include <unordered_set>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/physics/Physics.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"

namespace game {

// Scattered-prop collision for rocks and tree trunks. The scatter is a pure
// deterministic function (render::scatterProps), so collision re-runs it
// near the player and drops ONE static box per trunk / rock — no
// dependency on the render chunks, same TerrainCollision streaming
// pattern: the scatter runs on a WORKER (it is the expensive half), the
// main thread only creates the Jolt bodies from the returned collider
// list (3x3 chunk ring, hysteresis eviction). Purity makes a stale
// in-flight result byte-identical to a fresh one — drops are benign.
// Bushes stay walk-through on purpose.
class VegetationCollision {
public:
    // `jobs` null = synchronous cook (headless tests), one chunk per
    // update.
    VegetationCollision(phys::PhysicsWorld& physics,
                        const render::TerrainParams& params,
                        core::JobSystem* jobs = nullptr);
    ~VegetationCollision();

    VegetationCollision(const VegetationCollision&) = delete;
    VegetationCollision& operator=(const VegetationCollision&) = delete;

    // Converges the 3x3 chunk ring around `focus` (worker scatters, main
    // lands the bodies); evicts chunks more than one ring beyond it.
    void update(const Vec3& focus);

    u32 chunkCount() const { return static_cast<u32>(chunks.size()); }
    u32 bodyCount() const { return bodies; }

private:
    // One prop's body, precomputed on the worker: the main thread only
    // hands it to Jolt.
    struct Collider {
        Vec3 half {};
        Vec3 center {};
        f32 yaw { 0.0f }; // 0 = axis-aligned box
        bool oriented { false };
    };
    struct CookedChunk {
        u64 key { 0 };
        vector<Collider> colliders;
    };

    static CookedChunk cookColliders(const render::TerrainParams& params,
                                     i32 cx, i32 cz);
    void landChunk(CookedChunk&& cooked);

    phys::PhysicsWorld& physics;
    render::TerrainParams params;
    core::JobSystem* jobs { nullptr };
    sptr<core::ConcurrentQueue<CookedChunk>> built;
    std::unordered_set<u64> pending;
    std::unordered_map<u64, vector<phys::BodyId>> chunks; // packed coord
    u32 bodies { 0 };
};

} // namespace game
