#include "game/VegetationCollision.hpp"

#include <cmath>

#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/render/landscape/VegetationSystem.hpp"

namespace game {

namespace {

u64 packChunk(i32 x, i32 z) {
    return (static_cast<u64>(static_cast<u32>(x)) << 32) |
           static_cast<u64>(static_cast<u32>(z));
}

i32 unpackX(u64 key) { return static_cast<i32>(key >> 32); }
i32 unpackZ(u64 key) { return static_cast<i32>(key & 0xFFFFFFFFu); }

// Collider proportions, calibrated on the generators (TreeGenerator:
// trunk radius 0.17-0.24 x scale, height 4.2-6.1 x scale; rocks: ~unit
// boulder x scale). Slightly generous radii so the capsule never clips
// visible bark; a box is plenty — nobody circles a trunk with a caliper.
constexpr f32 kTrunkHalfXZ = 0.28f;
constexpr f32 kTrunkHalfY = 2.5f;
constexpr f32 kRockHalfXZ = 0.75f;
constexpr f32 kRockHalfY = 0.6f;

} // namespace

VegetationCollision::VegetationCollision(phys::PhysicsWorld& physics,
                                         const render::TerrainParams& params)
    : physics { physics }, params { params } {}

VegetationCollision::~VegetationCollision() {
    for (const auto& [key, chunkBodies] : chunks) {
        for (const phys::BodyId body : chunkBodies) {
            physics.removeBody(body);
        }
    }
}

void VegetationCollision::cookChunk(i32 cx, i32 cz) {
    // The SAME deterministic scatter the renderer draws — collision and
    // visuals can never disagree.
    const render::VegetationSystem::VariantBuckets buckets =
        render::scatterProps(params, cx, cz);
    vector<phys::BodyId>& out = chunks[packChunk(cx, cz)];
    const auto add = [&](const render::VegetationSystem::Instance& prop,
                         f32 halfXZ, f32 halfY) {
        const f32 s = prop.positionScale.w;
        const Vec3 base { prop.positionScale.x, prop.positionScale.y,
                          prop.positionScale.z };
        const phys::BodyId body = physics.addStaticBox(
            { halfXZ * s, halfY * s, halfXZ * s },
            base + Vec3 { 0.0f, halfY * s, 0.0f });
        if (body != 0) {
            out.push_back(body);
            ++bodies;
        }
    };
    for (u32 v = 0; v < render::VegetationSystem::kTreeVariants; ++v) {
        for (const auto& prop : buckets[v]) {
            add(prop, kTrunkHalfXZ, kTrunkHalfY);
        }
    }
    for (u32 v = render::VegetationSystem::kFirstRock;
         v < render::VegetationSystem::kFirstBush; ++v) {
        for (const auto& prop : buckets[v]) {
            add(prop, kRockHalfXZ, kRockHalfY);
        }
    }
    // Bushes (kFirstBush..) are deliberately walk-through.
}

void VegetationCollision::update(const Vec3& focus) {
    const f32 chunkSize = render::TerrainSystem::kChunkSize;
    const i32 centerX = static_cast<i32>(std::floor(focus.x / chunkSize));
    const i32 centerZ = static_cast<i32>(std::floor(focus.z / chunkSize));

    // One scatter re-run per update, nearest missing chunk first (the
    // TerrainCollision anti-stutter contract): the chunk underfoot lands
    // immediately, the ring converges over the next frames.
    i32 bestX = 0;
    i32 bestZ = 0;
    i32 bestDist = INT32_MAX;
    for (i32 tz = centerZ - 1; tz <= centerZ + 1; ++tz) {
        for (i32 tx = centerX - 1; tx <= centerX + 1; ++tx) {
            if (chunks.contains(packChunk(tx, tz))) {
                continue;
            }
            const i32 dist = (tx - centerX) * (tx - centerX) +
                             (tz - centerZ) * (tz - centerZ);
            if (dist < bestDist) {
                bestDist = dist;
                bestX = tx;
                bestZ = tz;
            }
        }
    }
    if (bestDist != INT32_MAX) {
        cookChunk(bestX, bestZ);
    }

    // Evict beyond ring 2 (hysteresis).
    for (auto it = chunks.begin(); it != chunks.end();) {
        const i32 dx = unpackX(it->first) - centerX;
        const i32 dz = unpackZ(it->first) - centerZ;
        if (dx < -2 || dx > 2 || dz < -2 || dz > 2) {
            for (const phys::BodyId body : it->second) {
                physics.removeBody(body);
            }
            bodies -= static_cast<u32>(it->second.size());
            it = chunks.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace game
