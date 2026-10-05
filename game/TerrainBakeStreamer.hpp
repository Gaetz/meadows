#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <unordered_set>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/render/landscape/WaterSystem.hpp"
#include "game/MapBaker.hpp"
#include "engine/terrain/generation/TileBake.hpp"

namespace game {

// Water sidecar next to a slice's .trg (lakes with their basin masks +
// river polylines) — the streamer's cache format, shared with the map
// baker (docs/PAYSAGE.md §1.1).
bool writeWaterFile(const std::filesystem::path& path,
                    const vector<render::terraingen::Lake>& lakes,
                    const vector<render::terraingen::River>& rivers);
bool readWaterFile(const std::filesystem::path& path,
                   vector<render::terraingen::Lake>& lakes,
                   vector<render::terraingen::River>& rivers);

// The far-water provider's data pass: the lakes/rivers of every cached
// .twb of the active map inside the square (baked slices keep their
// REAL water even far away), plus the master-network fleuves wherever
// no slice is cached yet. Pure and worker-callable: file reads + the
// memoized master network. Lakes are downsampled to ~64 m coarse masks.
// `grid`: the border-transition lattice — master-fleuve ribbons are
// cut where a border reshaped the analytic ground they were routed on
// (a sea arm drowned it, or a range buried it), so no course floats
// over a channel the fallback terrain shows drowned.
render::WaterSystem::FarWaterSet collectFarWater(
    const std::filesystem::path& cacheRoot, i32 mapX, i32 mapZ,
    f32 tileSize,
    const render::terraingen::ProceduralControlParams& controls,
    const render::terraingen::MacroParams& macro,
    const render::terraingen::MasterNetworkParams& net,
    const render::terraingen::MapGridSpec& grid, f32 seaLevel,
    f32 cx, f32 cz, f32 halfSpan);

// Point query on a far-water set: inside a coarse lake mask or within
// a fleuve ribbon's half-width. The spawn probe's water oracle before
// any slice of the map is published (sea level is the caller's).
bool farWaterWetAt(const render::WaterSystem::FarWaterSet& set, f32 x,
                   f32 z);

// Bounded-map slice streamer (chantier CARTES; the windowed per-tile
// bake path died in M1.5b): slices of the ACTIVE map are READ from the
// map cache on workers and handed to the scene for publication into
// TerrainParams.base; a request into an unbaked map defers the tile
// and kicks ONE background game::bakeMap. Same mailbox pattern as
// TerrainCollision: workers push, the frame thread drains — the ECS
// world and the GPU never leave the main thread.
class TerrainBakeStreamer {
public:
    struct PublishedTile {
        render::TerrainRegion region;
        vector<render::terraingen::Lake> lakes;
        vector<render::terraingen::River> rivers;
        i32 tx { 0 };
        i32 tz { 0 };
    };

    // Bounded-map streaming (chantier CARTES M1.4, docs/PAYSAGE.md §1.1):
    // slices are READ from <cacheDir>/map_<mx>_<mz>/ — a request into an
    // unbaked map defers the tile and kicks ONE background map bake
    // (game::bakeMap on a worker; the deferred tiles occupy no worker,
    // so the bake's own slice jobs cannot starve). Requests outside the
    // active map rect are dropped (the M3.2 clamp — ringStatus counts
    // only in-rect tiles or the warmup gate never completes at a rim).
    struct MapStreamConfig {
        i32 tilesPerSide { kMapTilesPerSide };
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
    // gate holds on this while the map bakes.
    u32 pendingCount() const {
        return static_cast<u32>(pending.size());
    }

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
    void drain(const std::function<void(PublishedTile&&)>& publish);
};

} // namespace game
