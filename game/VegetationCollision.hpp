#pragma once

#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/physics/Physics.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"

namespace game {

// Scattered-prop collision for rocks and tree trunks. The scatter is a pure
// deterministic function (render::scatterProps), so collision re-runs it
// on the CPU near the player and drops ONE static box per trunk / rock —
// no dependency on the render chunks, same TerrainCollision streaming
// pattern (3x3 chunk ring, one chunk cooked per update, hysteresis
// eviction). Bushes stay walk-through on purpose.
class VegetationCollision {
public:
    VegetationCollision(phys::PhysicsWorld& physics,
                        const render::TerrainParams& params);
    ~VegetationCollision();

    VegetationCollision(const VegetationCollision&) = delete;
    VegetationCollision& operator=(const VegetationCollision&) = delete;

    // Converges the 3x3 chunk ring around `focus` (one scatter re-run per
    // call); evicts chunks more than one ring beyond it.
    void update(const Vec3& focus);

    u32 chunkCount() const { return static_cast<u32>(chunks.size()); }
    u32 bodyCount() const { return bodies; }

private:
    void cookChunk(i32 cx, i32 cz);

    phys::PhysicsWorld& physics;
    render::TerrainParams params;
    std::unordered_map<u64, vector<phys::BodyId>> chunks; // packed coord
    u32 bodies { 0 };
};

} // namespace game
