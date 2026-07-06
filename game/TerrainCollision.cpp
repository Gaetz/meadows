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

    // Build the 3x3 ring.
    vector<f32> samples;
    for (i32 tz = centerZ - 1; tz <= centerZ + 1; ++tz) {
        for (i32 tx = centerX - 1; tx <= centerX + 1; ++tx) {
            const u64 key = packTile(tx, tz);
            if (tiles.contains(key)) {
                continue;
            }
            const Vec3 origin { static_cast<f32>(tx) * kTileEdge, 0.0f,
                                static_cast<f32>(tz) * kTileEdge };
            samples.resize(static_cast<size_t>(kSamples) * kSamples);
            for (u32 row = 0; row < kSamples; ++row) {
                for (u32 col = 0; col < kSamples; ++col) {
                    samples[row * kSamples + col] = render::terrain::height(
                        params, origin.x + static_cast<f32>(col) * kSpacing,
                        origin.z + static_cast<f32>(row) * kSpacing);
                }
            }
            tiles.emplace(key, physics.addHeightField(samples.data(),
                                                      kSamples, origin,
                                                      kSpacing));
        }
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
