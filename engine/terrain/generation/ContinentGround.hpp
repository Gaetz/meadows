#pragma once

#include <atomic>
#include <filesystem>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// The CONTINENT GROUND — stage 0 bis of the map pipeline
// (docs/PAYSAGE.md §1.3, §7.9): the stage-1 recipe (macro, imprint,
// fluvial + thermal erosion) and the outlet piercing run ONCE over a
// whole master super-cell (one continent: its 49 km core plus the
// network apron) at the maps' own stage-1 texel, cached on disk next to
// the master networks. A map's stage-1 is then a CROP of it: every map
// of the continent reads one erosion, one drainage, one set of gorges,
// whatever the order the maps are baked in — a map line is a seam of
// the bounded-support finalize only. The per-map erosion of a window
// (an apron of 3 km, its rim the base level) could never agree with its
// neighbour's: measured 34 m RMS / 317 m max across an Open line, the
// cliffs ON the lines the player saw.
// The runtime horizon reads a 64 m OVERVIEW of it past the baked map
// overviews (SandboxTerrain): the far ground beyond an unbaked
// neighbour is the carved continent, not the uncarved analytic.
// Pure function of (bake params, map size, super cell): bit-identical
// for every caller, memoized in process and on disk.

namespace render::terraingen {

struct TileBakeParams;

struct ContinentGroundParams {
    // Off = every map erodes alone (the legacy pipeline, the A/B).
    bool enabled { true };
    REFLECT_BEGIN(ContinentGroundParams, void)
        REFLECT_FIELD(enabled)
    REFLECT_END()
};
u64 hashKey(const ContinentGroundParams& p);

struct ContinentGround {
    GridSpec grid;
    vector<f32> height;  // eroded + pierced
    vector<f32> deposit; // fluvial + thermal sediment (m), the mask input
    bool valid() const {
        return grid.n >= 2 && height.size() == grid.cells();
    }
    // Bilinear sample; false outside the grid.
    bool heightAt(f32 x, f32 z, f32& out) const;
};

// The 64 m decimation of a continent (the overview lattice the map
// overviews share), for the runtime horizon.
struct ContinentOverview {
    GridSpec grid;
    vector<f32> height;
};

// The super cell holding world coordinate `v` along one axis (the
// master lattice: superOrigin + k * superRegionSize).
i32 continentSuperIndex(const MasterNetworkParams& network, f32 v);

// The cache identity of a continent: every bake input (the params' key),
// the map size its border lattice is shaped with, the file format.
// game::bakeMap and the runtime loader pass the SAME unmodified game
// params and the same map size, so they name the same files.
u64 continentGroundKey(const TileBakeParams& params, f32 mapSize);

// The continent ground of super cell (superX, superZ): the process memo,
// else the disk cache (the master-network cache dir), else computed and
// written (ground + overview). `cancel` raised = nullptr, nothing
// cached. Minutes and gigabytes once per continent (4097² at 16 m).
sptr<const ContinentGround> continentGroundFor(
    const TileBakeParams& params, f32 mapSize, i32 superX, i32 superZ,
    const std::atomic<bool>* cancel = nullptr);

// READ-ONLY lookups: the cached files under `dir` (or the memo), nullopt
// when the continent is not baked yet. Never compute. The overview is
// what the scene polls between frames; the full ground is for the
// baker's instruments.
std::optional<ContinentGround> loadContinentGround(
    const std::filesystem::path& dir, const TileBakeParams& params,
    f32 mapSize, i32 superX, i32 superZ);
std::optional<ContinentOverview> loadContinentOverview(
    const std::filesystem::path& dir, const TileBakeParams& params,
    f32 mapSize, i32 superX, i32 superZ);

} // namespace render::terraingen
