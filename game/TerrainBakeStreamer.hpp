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
#include "engine/render/landscape/WaterSystem.hpp"
#include "engine/terrain/generation/TileBake.hpp"

namespace game {

// E4a: the far-water provider's data pass — the lakes/rivers of every
// cached .twb inside the square (visited tiles keep their REAL water
// even far away), plus the master-network fleuves wherever no tile
// was ever baked. Pure and worker-callable: file reads + the memoized
// master network. Lakes are downsampled to ~64 m coarse masks.
// Water sidecar next to a tile's .trg (lakes with their basin masks +
// river polylines) — the streamer's cache format, shared with the map
// baker (docs/TERRAIN-MAPS.md).
bool writeWaterFile(const std::filesystem::path& path,
                    const vector<render::terraingen::Lake>& lakes,
                    const vector<render::terraingen::River>& rivers);
bool readWaterFile(const std::filesystem::path& path,
                   vector<render::terraingen::Lake>& lakes,
                   vector<render::terraingen::River>& rivers);

// `grid`: the border-transition lattice — master-fleuve ribbons are
// cut where a border reshaped the analytic ground they were routed on
// (a sea arm drowned it, or a range buried it), so no course floats
// over a channel the fallback terrain shows drowned.
render::WaterSystem::FarWaterSet collectFarWater(
    const std::filesystem::path& cacheDir, f32 tileSize,
    const render::terraingen::ProceduralControlParams& controls,
    const render::terraingen::MacroParams& macro,
    const render::terraingen::MasterNetworkParams& net,
    const render::terraingen::MapGridSpec& grid, f32 seaLevel,
    f32 cx, f32 cz, f32 halfSpan);

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

    // Bounded-map streaming (chantier CARTES M1.4, docs/TERRAIN-MAPS.md):
    // slices are READ from <cacheDir>/map_<mx>_<mz>/ — a request into an
    // unbaked map defers the tile and kicks ONE background map bake
    // (game::bakeMap on a worker; the deferred tiles occupy no worker,
    // so the bake's own slice jobs cannot starve). Requests outside the
    // active map rect are dropped (the M3.2 clamp — ringStatus counts
    // only in-rect tiles or the warmup gate never completes at a rim).
    struct MapStreamConfig {
        bool enabled { false };
        i32 tilesPerSide { 6 };
        i32 mapX { 0 }; // the active map
        i32 mapZ { 0 };
        // Border transitions on (the grid spec derives from the
        // world seed + lattice in the ctor).
        bool borders { true };
    };

    TerrainBakeStreamer(const render::terraingen::TileBakeParams& params,
                        std::filesystem::path cacheDir,
                        core::JobSystem* jobs = nullptr,
                        MapStreamConfig map = {});

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
    // Pre-bake service (cooker pre-bake): request every tile whose
    // rect overlaps [minX,maxX]x[minZ,maxZ], then pump drain() until
    // pendingCount() reaches zero. Cached tiles publish via the fast
    // cache-read path — re-running over a warm cache is cheap.
    void requestRect(f32 minX, f32 minZ, f32 maxX, f32 maxZ);
    // Drain finished bakes only — update() without the focus-driven
    // desired-set policy.
    void drain(const std::function<void(PublishedTile&&)>& publish);
    // The scene evicted this tile's region: re-request it on return.
    void forgetTile(i32 tx, i32 tz) { published.erase(keyOf(tx, tz)); }

    // Background-bakes ANOTHER map (the approach prefetch, chantier
    // CARTES M4.3): the player nearing a rim warms the neighbour so the
    // crossing costs a fade, not a bake. Shares the one-bake-in-flight
    // gate with the active map. No-op when already baked or busy.
    void prefetchMap(i32 mapX, i32 mapZ);

private:
    static u64 keyOf(i32 tx, i32 tz) {
        return (static_cast<u64>(static_cast<u32>(tx)) << 32) |
               static_cast<u64>(static_cast<u32>(tz));
    }
    void request(i32 tx, i32 tz);

    render::terraingen::TileBakeParams params;
    std::filesystem::path cacheDir;
    core::JobSystem* jobs { nullptr };
    MapStreamConfig map;
    // Tiles waiting for their map's bake to land (main thread only).
    std::unordered_set<u64> deferredForMap;
    // One map bake in flight, ever (shared: the worker clears it).
    std::shared_ptr<std::atomic<bool>> mapBaking {
        std::make_shared<std::atomic<bool>>(false)
    };
    u32 manifestCheckCountdown { 0 };
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
