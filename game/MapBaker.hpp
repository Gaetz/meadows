#pragma once

#include <filesystem>
#include <functional>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TileBake.hpp"

namespace core {
class JobSystem;
}

namespace game {

// The bounded-map baker (docs/TERRAIN-MAPS.md, chantier CARTES M1):
// ONE global erosion for the whole map — stage-1 on a map-sized window
// — then every 4096 m slice finalized against that single shared
// surface (the map stage-1 stands in for all nine 3x3 neighbours, so
// interior slice borders are derived from identical ground by
// construction; measured 0-8 m of band divergence vs 200-441 m for
// independently-windowed tiles). Slices ship in the streamer's own
// cache format (tile_<tx>_<tz>_v<N>.trg + .twb) inside
// <cacheDir>/map_<mx>_<mz>/ next to a manifest, so the runtime can
// stream a baked map with the existing machinery.
constexpr u32 kMapBakeVersion = 1;
constexpr i32 kMapTilesPerSide = 6; // 6 x 4096 m = 24576 m — one
                                    // MasterNetwork super-region

struct MapBakeStats {
    f64 stage1Seconds { 0.0 };
    f64 sliceSeconds { 0.0 };
    u32 slicesWritten { 0 };
    bool cancelled { false };
};

// Bakes map (mapX, mapZ) into <cacheDir>/map_<mx>_<mz>/ (created).
// `params` is the game's TileBakeParams (tileSize = the slice size);
// the map window derives from it (tileSize * tilesPerSide, apron =
// kBasinResolveMargin so every slice's canonical-basin window is
// covered). Slices run on `jobs` workers when given (the map stage-1
// is shared read-only), synchronously otherwise. `progress` (nullable)
// is called from the calling thread as slices land: (done, total).
// Cancellation: the JobSystem stop flag — a cancelled bake writes no
// manifest (partial slices are orphaned by the missing manifest and
// overwritten on the next run).
MapBakeStats bakeMap(const render::terraingen::TileBakeParams& params,
                     i32 mapX, i32 mapZ,
                     const std::filesystem::path& cacheDir,
                     core::JobSystem* jobs = nullptr,
                     i32 tilesPerSide = kMapTilesPerSide,
                     const std::function<void(u32, u32)>& progress = {});

// The deterministic edge-style rule for procedural maps (chantier
// CARTES): east/west borders are RIDGES, north/south are SEA — the
// rule is symmetric by construction, so the two maps sharing a border
// always agree on its style (map (0,0).east == map (1,0).west).
// Authored maps override per WorldspaceForm (M5).
render::terraingen::MapEdgeSpec mapEdgeStylesFor(i32 mapX, i32 mapZ);

// The map's cache directory under the seed cache root.
std::filesystem::path mapCacheDir(const std::filesystem::path& cacheDir,
                                  i32 mapX, i32 mapZ);

// The map OVERVIEW: the global stage-1 surface decimated to 64 m,
// written by bakeMap next to the slices (rim + 3 km of shaped apron
// included). It is the runtime fallback INSIDE a baked map — a
// pointwise analytic mirror cannot follow a globally carved valley
// network (measured -250..-456 m mean drift in the highlands), the
// map's own coarse truth can (upsampling error only).
struct MapOverview {
    render::terraingen::GridSpec grid;
    vector<f32> heights;
};
std::optional<MapOverview> loadMapOverview(
    const std::filesystem::path& mapDir);

// A map is usable only when its manifest MATCHES (coords, slice
// layout, bake version) — a stale dir from another configuration must
// re-bake, not half-load (existence alone lied: a 2x2 test map
// masqueraded as a 6x6 production one).
bool mapBakedAndValid(const std::filesystem::path& cacheDir, i32 mapX,
                      i32 mapZ, i32 tilesPerSide);

} // namespace game
