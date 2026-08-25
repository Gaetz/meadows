#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "engine/terrain/generation/Finalize.hpp"
#include "engine/terrain/generation/FluvialErosion.hpp"
#include "engine/terrain/generation/Hydrology.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"
#include "engine/terrain/generation/ThermalErosion.hpp"

// The pipeline for ONE sandbox tile, in TWO ORDERED STAGES (the
// Peytavie 2019 / UE Water lesson: water is a LATER pass over the FINAL
// terrain):
//   stage 1 — terrain only, per tile, deterministic, cached;
//   stage 2 — hydrology on the COMPOSED 3x3 neighbourhood terrain (the
//   same blend the runtime shows), then carve/masks/ownership for the
//   center tile. Two neighbours derive their shared-band water from the
//   SAME composite, so levels and courses agree by construction —
//   per-tile provisional hydrology floated over the blended ground.
//
// Vocabulary: a baked "tile" ships as a runtime render::TerrainRegion
// (tile + overlapMargin rect); the three halo widths are, inside out:
//   overlapMargin — ring KEPT in the published region, shared with the
//     neighbour bakes (height() blends it away by edge weight);
//   waterMargin — how far past the tile the stage-2 hydrology window
//     extends, so rivers/lakes continue across borders;
//   apron — stage-1 simulation ring, cropped away.

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
    // Extra simulated ring, cropped away. Sized against the RANGE
    // wavelength: big massifs span tiles, the apron is what makes both
    // sides carve (almost) the same valleys.
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
    FinalizeParams finalize;
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

TileStage1 bakeTileStage1(const TileBakeParams& params, i32 tx, i32 tz);

// `stage1At` must return the stage-1 of any tile in the 3x3
// neighbourhood. The CENTER is mandatory (asserted; null returns an
// empty result); a missing neighbour falls back to the center's sim,
// which covers the window thanks to the apron.
TileBakeResult bakeTileStage2(
    const TileBakeParams& params, i32 tx, i32 tz,
    const std::function<const TileStage1*(i32, i32)>& stage1At);

// Convenience: both stages, computing the 3x3 stage-1s inline (tests,
// the editor tool). The streamer caches stage-1s instead.
TileBakeResult bakeTile(const TileBakeParams& params, i32 tx, i32 tz);

// Cache identity, split by stage so a hydrology/finalize change does not
// invalidate the expensive stage-1 terrain caches:
//   kStage1Version   — bump when stage-1 output changes (S1 macro, S2
//     fluvial, S3 thermal, or their defaults);
//   kTileBakeVersion — bump when ANY published output changes (stage-2
//     included; a stage-1 bump implies bumping this one too).
// Miss either and stale caches keep the old landscape.
constexpr u32 kStage1Version = 42;
constexpr u32 kTileBakeVersion = 47;

// Wider flood window for CANONICAL BASIN resolution: a lake touching
// the hydrology-window rim is re-flooded on tile +/- this margin so its
// true spill level and full mask replace the truncated view. Must stay
// within the 3x3 stage-1 coverage (< tileSize).
constexpr f32 kBasinResolveMargin = 3072.0f;

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
