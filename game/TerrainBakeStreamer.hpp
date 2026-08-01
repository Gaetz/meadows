#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/terrain/generation/TileBake.hpp"

namespace game {

// Sandbox terrain streamer: bakes 4 km tiles around the focus on
// workers, through the two-stage TileBake pipeline — stage 1 (terrain
// only, per tile) is disk-cached and deduplicated across workers via
// Stage1Registry; stage 2 derives the water from the composed 3x3
// neighbourhood and finalizes the center tile. Finished tiles are
// cached on disk keyed (worldSeed, tile, pipeline version) and handed
// to the scene for publication into TerrainParams.base.
// Same mailbox pattern as TerrainCollision: workers push, the frame
// thread drains — the ECS world and the GPU never leave the main thread.
class TerrainBakeStreamer {
public:
    struct PublishedTile {
        render::TerrainRegion region;
        vector<render::terraingen::Lake> lakes;
        vector<render::terraingen::River> rivers;
        i32 tx { 0 };
        i32 tz { 0 };
    };

    TerrainBakeStreamer(const render::terraingen::TileBakeParams& params,
                        std::filesystem::path cacheDir,
                        core::JobSystem* jobs = nullptr);

    // Converges the desired tile set around `focus` (its tile + any tile
    // within prefetch reach), drains finished bakes into `publish` on the
    // calling (frame) thread. Without a JobSystem, bakes run synchronous
    // — headless tests only.
    void update(const Vec3& focus,
                const std::function<void(PublishedTile&&)>& publish);

    u32 publishedCount() const {
        return static_cast<u32>(published.size());
    }
    // Tiles requested but not yet handed to publish() — the loading
    // gate holds on this (a first-boot stage-1 bake takes seconds).
    u32 pendingCount() const {
        return static_cast<u32>(pending.size());
    }
    // Unique stage-1 bakes completed (computed or cache-read) since
    // startup — the loading gate's FINE progress signal: a tile hides
    // up to nine of these, each seconds long on a cold cache.
    u32 stage1Count() const;

    // Ring completeness around `focus`: how many tiles the prefetch
    // square needs there vs how many are published. The warmup state
    // machine (boot, travel, the spectator catch-up bar) reads this —
    // ONE source for "is this place generated".
    struct RingStatus {
        u32 needed { 0 };
        u32 published { 0 };
    };
    RingStatus ringStatus(const Vec3& focus) const;
    f32 tileSize() const { return params.tileSize; }
    // The scene evicted this tile's region: re-request it on return.
    void forgetTile(i32 tx, i32 tz) { published.erase(keyOf(tx, tz)); }

private:
    static u64 keyOf(i32 tx, i32 tz) {
        return (static_cast<u64>(static_cast<u32>(tx)) << 32) |
               static_cast<u64>(static_cast<u32>(tz));
    }
    void request(i32 tx, i32 tz);

    render::terraingen::TileBakeParams params;
    std::filesystem::path cacheDir;
    core::JobSystem* jobs { nullptr };
    f32 prefetchReach { 1408.0f }; // beyond the view ring, before FarTerrain
    Vec3 lastFocus { 0.0f };       // for the heading-biased request order
    // Workers push, the frame thread drains; shared_ptr so in-flight
    // bakes outlive a teardown harmlessly (TerrainCollision postmortem).
    std::shared_ptr<core::ConcurrentQueue<PublishedTile>> built;
    std::unordered_set<u64> published;
    std::unordered_set<u64> pending;

public:
    // Stage-1 dedup across workers: adjacent tile jobs need overlapping
    // 3x3 neighbourhoods — without this registry they RACED to compute
    // the same stage-1s (9x the work, minutes of first-boot lag). One
    // worker computes, the others wait on it; results stay in memory
    // (bounded) and on disk.
    struct Stage1Registry {
        std::mutex mutex;
        std::condition_variable ready;
        std::unordered_map<u64,
                           sptr<const render::terraingen::TileStage1>>
            done;
        std::unordered_set<u64> inflight;
        std::atomic<u32> completed { 0 }; // monotone, for progress UIs
    };

private:
    std::shared_ptr<Stage1Registry> stage1s {
        std::make_shared<Stage1Registry>()
    };
};

} // namespace game
