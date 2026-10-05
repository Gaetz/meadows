#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "engine/terrain/generation/Finalize.hpp"
#include "engine/terrain/generation/FluvialErosion.hpp"
#include "engine/terrain/generation/Hydrology.hpp"
#include "engine/terrain/generation/MasterNetwork.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"
#include "engine/terrain/generation/ThermalErosion.hpp"
#include "engine/terrain/generation/WaterSolve.hpp"

// The bake pipeline of ONE BOUNDED MAP (docs/PAYSAGE.md §1.3), in
// ordered passes over the whole map (the Peytavie 2019 / UE Water
// lesson: water is a LATER pass over the FINAL terrain):
//   stage 1 — bakeTileStage1: macro + erosion on the map window
//     (map rect + apron), once per map, deterministic;
//   hydrology — extractMapHydrology: lakes/rivers/tiers routed ONCE on
//     that single surface, so every slice carves toward the same water;
//   slices — bakeMapSlice: fine upsample, fine erosion, carves, masks
//     and the lake/river reconcile for ONE slice rect, in parallel.
// bakeSoloTile is the same pipeline on a 1x1-slice map (benches/tests).
//
// Vocabulary: a baked "slice" (historically "tile") ships as a runtime
// render::TerrainRegion (slice + overlapMargin rect); the halo widths
// are, inside out:
//   overlapMargin — ring KEPT in the published region, shared with the
//     neighbour slices (height() blends it away by edge weight; slices
//     of one map are bit-identical there by construction);
//   waterMargin — how far past the map rect the hydrology window
//     extends;
//   apron — stage-1 simulation ring past the map rect, cropped away
//     (kMapApron in production, see below).

namespace render::terraingen {

// Per-biome erosion character, indexed by the biome palette contract
// (TerrainGen.hpp: 0 temperate, 1 arid, 2 alpine, 3 tundra,
// 4 subalpine, 5 steppe). The
// vegetation-cohesion idea: temperate cover holds soil together, arid
// terrain gullies deep, alpine rock stands steeper. capacityScale and
// fineScale feed the sediment-deposition and fine-erosion passes.
struct BiomeErosion {
    f32 erodibility { 1.0f };   // scales the fluvial k per texel
    f32 talusScale { 1.0f };    // scales the thermal angle of repose
    f32 capacityScale { 1.0f }; // scales sediment capacity
    f32 fineScale { 1.0f };     // scales the fine-erosion carve
};

struct TileBakeParams {
    u32 worldSeed { 1337 };
    f32 tileSize { 4096.0f };
    // Extra simulated ring past the map rect, cropped away; its rim is
    // the erosion base level. This default is the bench/test value —
    // game::bakeMap overrides it with kMapApron.
    f32 apron { 1536.0f };
    f32 overlapMargin { 64.0f };  // kept ring shared with neighbours
    // Erosion/hydrology grid resolution. This is the FREQUENCY of the
    // fastscape dissection: ridge-valley spacing scales with it (the
    // finalize chain re-details at 4 m / 2 m either way), so it is the
    // knob that spreads the same relief over fewer, broader ups and
    // downs.
    f32 macroTexel { 16.0f };
    // Stage-2 hydrology window: tile + this margin, sampled from the
    // composed neighbourhood terrain.
    f32 waterMargin { 1024.0f };
    ProceduralControlParams controls; // .seed overwritten by worldSeed
    MacroParams macro;
    FluvialParams fluvial;
    ThermalParams thermal;
    RidgeRoundParams rounding; // crest relaxation, uplift-gated
    HydrologyParams hydrology;
    // Stage-0 master network (fleuve promotion in S4 — the true
    // drainage areas the tile window cannot know).
    MasterNetworkParams network;
    // The fleuve imprint (S1): the master courses are CONSTRUCTED into
    // the macro before erosion — channel, alluvial plain, monotone bed.
    MasterImprintParams imprint;
    // Bounded-map border transitions (chantier CARTES v2): shaped
    // into the macro BEFORE the imprint and the erosion. .valid =
    // false = no transitions (the windowed path never sets it).
    MapGridSpec mapGrid;
    FinalizeParams finalize;
    // Solver parameters shared with the sim's boundary sources and the
    // offline oracle (the per-tile option-D solve itself is purged —
    // the real-time windowed sim owns runtime water).
    WaterSolveParams waterSolve { .rainRate = 4.0e-6f };
    // Measured fine-erosion reintroduction (B6) — the carved-rock
    // character coming back on the slopes without re-hatching the
    // socles. All defaults are the LEGACY behavior (bit-exact); the
    // erosion bench (cooker erosion-bench) explores the variants.
    //   fineCalmGate*: the fine-erosion damp reads
    //     smoothstep01(low, high, calm) instead of raw calm, so the
    //     mid-calm halo (0.3-0.6) stops blanketing the slopes.
    //     high <= 0 = legacy raw-calm damp.
    f32 fineCalmGateLow { 0.35f };
    f32 fineCalmGateHigh { 0.7f };
    //   fineSlopeReturn: extra fineScale on steep, non-calm ground
    //     (x(1 + r*steep*(1-calmGated))). 0 = off.
    f32 fineSlopeReturn { 0.35f };
    //   relaxGate*: the calm relaxation strength reads
    //     smoothstep01(low, high, calm) instead of raw calm — the
    //     mid-slopes keep their carve. high <= 0 = legacy.
    f32 relaxGateLow { 0.5f };
    f32 relaxGateHigh { 0.85f };
    //   keepCrestFade: 0 = keep as-is; else the erosion keep fades to
    //     keep*(1-fade) OFF the local crests (crest = stands above the
    //     ~500 m mean), matching the measured profile: erosion belongs
    //     to the mid-slopes, the summits only need light shaping.
    f32 keepCrestFade { 0.35f };
    // Indexed by biome palette id; empty = neutral everywhere. Ids past
    // the table's end fall back to neutral.
    vector<BiomeErosion> biomeErosion {
        { 0.9f, 1.0f, 1.15f, 0.8f },  // temperate
        { 1.3f, 0.85f, 0.7f, 1.5f },  // arid
        { 0.75f, 1.25f, 1.0f, 1.0f }, // alpine
        { 1.0f, 0.9f, 1.2f, 0.7f },   // tundra
        { 0.85f, 1.1f, 1.05f, 0.9f }, // subalpine (temperate->alpine mid)
        { 1.15f, 0.9f, 0.85f, 1.25f }, // steppe (temperate->arid mid)
    };
};

// Stage-1 output: the tile's eroded coarse terrain + the macro fields
// the finalize masks need. Deterministic per (params, tile).
struct TileStage1 {
    GridSpec sim;
    vector<f32> eroded;  // S3 output
    vector<f32> uplift;  // [0,1] orogeny field (fine-erosion lowland damp)
    vector<f32> deposit; // fluvial + thermal sediment (m) — mask material
    vector<f32> seaDist; // macro coast field (beach mask)
    vector<u8> biome;    // macro biome ids
    vector<f32> gentle;  // passability corridors (fine-erosion damp)
    vector<f32> calm;    // calm-socle family, control calm fused with
                         //   post-erosion valley floors (erosion damp)
    vector<f32> trunk;   // master-valley floorness (fleuve promotion,
                         //   site scoring)
};

struct TileBakeResult {
    TerrainRegion region; // cropped to tile + margin, masks included
    vector<Lake> lakes;   // world coordinates, tile-interior only
    vector<River> rivers;
};

// Per-texel erosion-character grids resolved from the biome id grid and
// the BiomeErosion table, box-blurred so erosion sees no seam at biome
// borders (ids are nearest-sampled). Empty table -> empty grids.
struct BiomeCharacter {
    vector<f32> erodibility;
    vector<f32> talusScale;
    vector<f32> capacityScale;
    vector<f32> fineScale;
};
BiomeCharacter biomeCharacter(const GridSpec& spec,
                              const vector<u8>& biome,
                              const vector<BiomeErosion>& table);

// `cancel` (all three functions below): when set and raised, the bake
// aborts at the next pass/iteration boundary and returns a PARTIAL
// result — shutdown only. Callers must discard it: never cache it to
// disk, never publish it to the streamer.
TileStage1 bakeTileStage1(const TileBakeParams& params, i32 tx, i32 tz,
                          const std::atomic<bool>* cancel = nullptr);

// ONE tile through the PRODUCTION (map) pipeline: a 1x1-slice map —
// stage-1 with a basin-resolving apron, the shared map hydrology, one
// slice finalize. Benches, oracles and tests bake through this so they
// exercise exactly what ships. (The windowed per-tile stage-2 — 3x3
// composite, per-tile hydrology, canonical basin resolution, anchor
// ownership — died with the windowed streamer path, chantier CARTES
// M1.5b, docs/PAYSAGE.md §1.1.)
TileBakeResult bakeSoloTile(const TileBakeParams& params, i32 tx,
                            i32 tz,
                            const std::atomic<bool>* cancel = nullptr);

// --- Bounded-map path (chantier CARTES, docs/PAYSAGE.md §1.1). The
// map is eroded ONCE (a map-sized stage-1); its hydrology is derived
// ONCE over the whole map window; every slice then finalizes against
// that same surface AND the same routed water. Per-slice hydrology
// windows carved toward different water surfaces — up to ~100 m of
// band divergence measured; sharing the hydrology is what makes slice
// borders agree to the fine-erosion residual.

struct MapHydrology {
    GridSpec window;    // map rect + waterMargin, at macroTexel
    vector<f32> ground; // the map surface on `window` (reconcile
                        //   fallback outside a slice's rect)
    HydrologyResult hydro;
};

// `mapS1.sim` must cover `window` (bake the map stage-1 with apron >=
// waterMargin; production uses kMapApron). There is no cross-slice
// truncation inside a map — basins clipped at the MAP window rim are
// identical for every slice (the border transitions make the rim
// sea/ridge).
MapHydrology extractMapHydrology(const TileBakeParams& params,
                                 const TileStage1& mapS1, i32 mapX,
                                 i32 mapZ, i32 tilesPerSide,
                                 const std::atomic<bool>* cancel =
                                     nullptr);

// Finalizes ONE slice of the map against the shared surface and
// hydrology. Lake ownership = bbox center in the slice rect (no anchor
// machinery — one solve, one identity per basin).
TileBakeResult bakeMapSlice(const TileBakeParams& params, i32 tx,
                            i32 tz, const TileStage1& mapS1,
                            const MapHydrology& mapHydro,
                            const std::atomic<bool>* cancel = nullptr);

// The hydrology floods lakes on the COMPOSITE terrain, BEFORE the
// finalize passes — and the discharge-driven fine erosion then carves
// gorges the flood never saw. A lake mask can therefore claim water
// over ground that no longer holds it (a floating slab pouring into a
// carved canyon, measured in-game at (7918, 597)). Run AFTER the
// region's final heights exist: per lake, a priority flood on the
// PUBLISHED ground finds the true spill of its deepest cell — the
// level is lowered to it (never raised), the mask re-cut to the cells
// actually enclosed, and a lake left shallower than ~0.5 m is
// dropped. Ground outside the region rect reads as the coarse
// `fallbackGround` on `fallbackSpec` when given (the map surface — the
// owner's mask survives past its slice rect), else as a wall
// (conservative: the old behavior cut every cross-border lake to its
// rect).
void reconcileLakesWithTerrain(vector<Lake>& lakes,
                               const render::TerrainRegion& region,
                               const GridSpec* fallbackSpec = nullptr,
                               const vector<f32>* fallbackGround =
                                   nullptr);

// Same debt for the RIVERS: their node surfaces come from the
// pre-finalize hydrology, and the carves (plus the lake reconcile
// above) can leave ribbons hanging over deepened beds or lowered
// lakes. The profile is REBUILT from the mouth upstream: surface =
// final ground + the tier's carved bed depth (the S5d formula),
// nodes inside a reconciled lake at its level, each raised by the
// downstream water (backwater) — monotone downhill by construction
// and always ABOVE the bed (a downstream running-min dived under
// every local bed rise: torrents flowing inside their channel
// walls). Ground outside the published rect keeps the baked surface.
void reconcileRiversWithTerrain(vector<River>& rivers,
                                const vector<Lake>& lakes,
                                const render::TerrainRegion& region,
                                const FinalizeParams& finalize);

// Does any lake's footprint still cover the neighbourhood of (x, z)
// (a dilated probe, ONE mask texel)? Used to drop lake-fed courses
// whose feeding lake RECEDED after reconciliation: the course was
// traced when the lake overflowed its old spill — keeping it paints
// a water slope down a col nothing feeds any more (measured dev at
// the spawn col).
bool lakeReachesPoint(const vector<Lake>& lakes, f32 x, f32 z);

// Cache identity of the published slices (.trg/.twb file names and the
// map manifest): bump when ANY published output changes — a stage-1,
// hydrology, finalize or default-parameter change alike. Miss it and
// stale caches keep the old landscape. The cache key is otherwise the
// world seed alone: a changed default needs this bump (or a cleared
// terrain-cache) to reach the player.
constexpr u32 kTileBakeVersion = 69;

// The production stage-1 apron of a map (game::bakeMap): the ring past
// the map rect that the erosion simulates and the rim basins resolve
// in, cropped away. Must cover the hydrology window (>= waterMargin).
constexpr f32 kMapApron = 3072.0f;

// Extra fine-window ring past the kept rect: the fine-erosion pass has
// bounded support (reach + receiver drift + thermal), and this halo
// keeps its edge effects outside what ships. Must exceed that support
// radius; overlapMargin + halo must stay within the apron.
constexpr f32 kFineErosionHalo = 192.0f;

// Runtime detail knobs stamped on every published region. The .trg asset
// does not carry them, so the disk-cache hit path (TerrainBakeStreamer)
// re-stamps these SAME constants — change them only here.
constexpr f32 kRegionDetailAmplitude = 0.35f;
constexpr f32 kRegionDetailWavelength = 5.0f;
constexpr i32 kRegionDetailOctaves = 2;

} // namespace render::terraingen
