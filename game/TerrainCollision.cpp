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

// The expensive part of a tile: 64x64 samples of the multi-octave terrain
// function (patches included). Deterministic — a stale in-flight result
// is byte-identical to a fresh one, so drops/races are benign.
vector<f32> sampleTile(const render::TerrainParams& params,
                       const Vec3& origin) {
    vector<f32> samples(static_cast<size_t>(TerrainCollision::kSamples) *
                        TerrainCollision::kSamples);
    for (u32 row = 0; row < TerrainCollision::kSamples; ++row) {
        for (u32 col = 0; col < TerrainCollision::kSamples; ++col) {
            samples[row * TerrainCollision::kSamples + col] =
                render::terrain::height(
                    params,
                    origin.x + static_cast<f32>(col) *
                                   TerrainCollision::kSpacing,
                    origin.z + static_cast<f32>(row) *
                                   TerrainCollision::kSpacing);
        }
    }
    return samples;
}

} // namespace

TerrainCollision::TerrainCollision(phys::PhysicsWorld& physics,
                                   const render::TerrainParams& params,
                                   core::JobSystem* jobs)
    : physics { physics }, params { params }, jobs { jobs },
      built { std::make_shared<core::ConcurrentQueue<SampledTile>>() } {}

TerrainCollision::~TerrainCollision() {
    for (const auto& [key, body] : tiles) {
        physics.removeBody(body);
    }
    // In-flight worker results die with the last `built` reference.
}

void TerrainCollision::addTile(u64 key, const Vec3& origin,
                               const f32* samples) {
    tiles.emplace(key,
                  physics.addHeightField(samples, kSamples, origin,
                                         kSpacing));
}

void TerrainCollision::cookSync(i32 tx, i32 tz) {
    const Vec3 origin { static_cast<f32>(tx) * kTileEdge, 0.0f,
                        static_cast<f32>(tz) * kTileEdge };
    const vector<f32> samples = sampleTile(params, origin);
    addTile(packTile(tx, tz), origin, samples.data());
}

bool TerrainCollision::request(i32 tx, i32 tz, bool& budget) {
    const u64 key = packTile(tx, tz);
    if (tiles.contains(key) || pending.contains(key)) {
        return false;
    }
    if (!jobs) {
        // Synchronous fallback (headless tests): one cook per update.
        if (!budget) {
            return false;
        }
        budget = false;
        cookSync(tx, tz);
        return true;
    }
    pending.insert(key);
    const Vec3 origin { static_cast<f32>(tx) * kTileEdge, 0.0f,
                        static_cast<f32>(tz) * kTileEdge };
    jobs->enqueue([queue = built, tileParams = params, key, origin] {
        queue->push({ key, origin, sampleTile(tileParams, origin) });
    });
    return false;
}

void TerrainCollision::update(const Vec3& focus) {
    const i32 centerX =
        static_cast<i32>(std::floor(focus.x / kTileEdge));
    const i32 centerZ =
        static_cast<i32>(std::floor(focus.z / kTileEdge));

    // Heading (XZ) from focus deltas — no dt needed, only a direction.
    Vec3 heading { 0.0f };
    if (hasLastFocus) {
        Vec3 d = focus - lastFocus;
        d.y = 0.0f;
        if (glm::dot(d, d) > 1e-6f) {
            heading = glm::normalize(d);
        }
    }
    lastFocus = focus;
    hasLastFocus = true;

    // 1. Land finished worker samples (a Jolt body add is cheap). A key no
    // longer pending was evicted while in flight — dropped; determinism
    // makes any such race benign.
    SampledTile done;
    while (built->tryPop(done)) {
        if (!pending.erase(done.key) || tiles.contains(done.key)) {
            continue;
        }
        addTile(done.key, done.origin, done.samples.data());
    }

    // 2. The tile underfoot is non-negotiable: with the heading prefetch
    // it is normally resident long before we cross; this synchronous cook
    // only fires on teleports/travel (and covers the async gap).
    if (!tiles.contains(packTile(centerX, centerZ))) {
        pending.erase(packTile(centerX, centerZ)); // in-flight? superseded
        cookSync(centerX, centerZ);
    }

    // 3. The 3x3 ring + one predicted tile along the heading, queued a
    // tile early so fast travel meets resident collision.
    bool budget = true; // sync-fallback: one cook per update
    for (i32 tz = centerZ - 1; tz <= centerZ + 1; ++tz) {
        for (i32 tx = centerX - 1; tx <= centerX + 1; ++tx) {
            request(tx, tz, budget);
        }
    }
    if (heading.x != 0.0f || heading.z != 0.0f) {
        const Vec3 ahead = focus + heading * (kTileEdge * 1.5f);
        request(static_cast<i32>(std::floor(ahead.x / kTileEdge)),
                static_cast<i32>(std::floor(ahead.z / kTileEdge)), budget);
    }

    // 4. Evict beyond ring 2 (hysteresis: a tile-border stroll never
    // churns); forget out-of-ring pending requests the same way.
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
    for (auto it = pending.begin(); it != pending.end();) {
        const i32 dx = unpackX(*it) - centerX;
        const i32 dz = unpackZ(*it) - centerZ;
        if (dx < -2 || dx > 2 || dz < -2 || dz > 2) {
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace game
