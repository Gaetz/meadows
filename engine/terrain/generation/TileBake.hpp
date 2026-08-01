#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/terrain/TerrainBase.hpp"
#include "engine/terrain/generation/Finalize.hpp"
#include "engine/terrain/generation/FluvialErosion.hpp"
#include "engine/terrain/generation/Hydrology.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"
#include "engine/terrain/generation/ThermalErosion.hpp"

// The pipeline for ONE sandbox super-tile, in TWO ORDERED STAGES (the
// Peytavie 2019 / UE Water lesson: water is a LATER pass over the FINAL
// terrain):
//   stage 1 — terrain only, per tile, deterministic, cached;
//   stage 2 — hydrology on the COMPOSED 3x3 neighbourhood terrain (the
//   same blend the runtime shows), then carve/masks/ownership for the
//   center tile. Two neighbours derive their shared-band water from the
//   SAME composite, so levels and courses agree by construction —
//   per-tile provisional hydrology floated over the blended ground.

namespace render::terraingen {

struct TileBakeParams {
    u32 worldSeed { 1337 };
    f32 tileSize { 4096.0f };
    // Extra simulated ring, cropped away. Sized against the RANGE
    // wavelength: big massifs span tiles, the apron is what makes both
    // sides carve (almost) the same valleys.
    f32 apron { 1536.0f };
    f32 overlapMargin { 64.0f };  // kept ring shared with neighbours
    f32 macroTexel { 8.0f };
    // Stage-2 hydrology window: tile + this margin, sampled from the
    // composed neighbourhood terrain.
    f32 waterMargin { 1024.0f };
    ProceduralControlParams controls; // .seed overwritten by worldSeed
    MacroParams macro;
    FluvialParams fluvial;
    ThermalParams thermal;
    HydrologyParams hydrology;
    FinalizeParams finalize;
};

// Stage-1 output: the tile's eroded coarse terrain + the macro fields
// the finalize masks need. Deterministic per (params, tile).
struct TileStage1 {
    GridSpec sim;
    vector<f32> eroded;  // S3 output
    vector<f32> seaDist; // macro coast field (beach mask)
    vector<u8> biome;    // macro biome ids
};

struct TileBakeResult {
    TerrainRegion region; // cropped to tile + margin, masks included
    vector<Lake> lakes;   // world coordinates, tile-interior only
    vector<River> rivers;
};

TileStage1 bakeTileStage1(const TileBakeParams& params, i32 tx, i32 tz);

// `stage1At` must return the stage-1 of any tile in the 3x3
// neighbourhood (center mandatory; a missing neighbour falls back to
// the center's sim, which covers the window thanks to the apron).
TileBakeResult bakeTileStage2(
    const TileBakeParams& params, i32 tx, i32 tz,
    const std::function<const TileStage1*(i32, i32)>& stage1At);

// Convenience: both stages, computing the 3x3 stage-1s inline (tests,
// the editor tool). The streamer caches stage-1s instead.
TileBakeResult bakeTile(const TileBakeParams& params, i32 tx, i32 tz);

// Cache identity: any change to the pipeline or its defaults that alters
// output must bump this, or stale caches keep the old landscape.
constexpr u32 kTileBakeVersion = 12;

} // namespace render::terraingen
