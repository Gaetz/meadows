#include "game/TerrainCollision.hpp"

#include <cmath>

namespace game {

namespace {

u64 packTile(i32 x, i32 z) {
    return (static_cast<u64>(static_cast<u32>(x)) << 32) |
           static_cast<u64>(static_cast<u32>(z));
}

i32 unpackX(u64 key) { return static_cast<i32>(key >> 32); }
i32 unpackZ(u64 key) { return static_cast<i32>(key & 0xFFFFFFFFu); }

} // namespace

TerrainCollision::TerrainCollision(phys::PhysicsWorld& physics,
                                   const render::TerrainParams& params)
    : physics { physics }, params { params } {}

TerrainCollision::~TerrainCollision() {
    for (const auto& [key, body] : tiles) {
        physics.removeBody(body);
    }
}

void TerrainCollision::update(const Vec3& focus) {
    const i32 centerX =
        static_cast<i32>(std::floor(focus.x / kTileEdge));
    const i32 centerZ =
        static_cast<i32>(std::floor(focus.z / kTileEdge));

    // Build the 3x3 ring — ONE tile per update, the focus tile first. A
    // tile is 4096 noise samples + a Jolt HeightFieldShape cook on the
    // main thread; cooking the whole leading edge (up to 3 tiles) in one
    // frame was part of the fast-travel stutter. The tile under the focus
    // always lands the frame it is needed; the diagonals catch up over the
    // next frames, long before anything can fall through them.
    i32 bestX = 0;
    i32 bestZ = 0;
    i32 bestDist = INT32_MAX;
    for (i32 tz = centerZ - 1; tz <= centerZ + 1; ++tz) {
        for (i32 tx = centerX - 1; tx <= centerX + 1; ++tx) {
            if (tiles.contains(packTile(tx, tz))) {
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
        const Vec3 origin { static_cast<f32>(bestX) * kTileEdge, 0.0f,
                            static_cast<f32>(bestZ) * kTileEdge };
        vector<f32> samples(static_cast<size_t>(kSamples) * kSamples);
        for (u32 row = 0; row < kSamples; ++row) {
            for (u32 col = 0; col < kSamples; ++col) {
                samples[row * kSamples + col] = render::terrain::height(
                    params, origin.x + static_cast<f32>(col) * kSpacing,
                    origin.z + static_cast<f32>(row) * kSpacing);
            }
        }
        tiles.emplace(packTile(bestX, bestZ),
                      physics.addHeightField(samples.data(), kSamples,
                                             origin, kSpacing));
    }

    // Evict beyond ring 2 (hysteresis: a tile-border stroll never churns).
    for (auto it = tiles.begin(); it != tiles.end();) {
        const i32 dx = unpackX(it->first) - centerX;
        const i32 dz = unpackZ(it->first) - centerZ;
        if (dx < -2 || dx > 2 || dz < -2 || dz > 2) {
            physics.removeBody(it->second);
            it = tiles.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace game
