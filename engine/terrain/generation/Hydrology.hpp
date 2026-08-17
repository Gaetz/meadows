#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Stage S4 — hydrology extraction + conditioning. Lakes and rivers are
// READ OFF the eroded surface (the Far Cry 5 freshwater model): lakes =
// depressions the priority flood had to fill, their level being the
// spill elevation (altitude lakes with their own level fall out for
// free); rivers = cells whose drainage area crosses a threshold, traced
// downstream. The raw courses are then conditioned for presentation:
// proximity merges, relaxation + spline smoothing, width character, and
// small dug ponds over confluences/hairpins — the two spots ribbon
// geometry handles badly.

namespace render::terraingen {

struct Lake {
    f32 level { 0.0f }; // water surface (spill elevation)
    u32 cells { 0 };
    // Footprint bounds, world meters.
    f32 minX { 0.0f };
    f32 minZ { 0.0f };
    f32 maxX { 0.0f };
    f32 maxZ { 0.0f };
    // Bbox-local cell mask of the ACTUAL flooded basin (1 = water) —
    // the render/query surface follows it, never the bbox rectangle: a
    // rectangle at lake level floats in the air over anything lower
    // inside the bounds.
    u32 maskWidth { 0 };
    u32 maskHeight { 0 };
    f32 maskTexel { 8.0f };
    vector<u8> mask;
    // 1 = a POND this pipeline placed (confluences, hairpins): its basin
    // does not exist in the eroded terrain — the finalize pass DIGS it.
    // Natural lakes (0) already sit in their depression.
    // The basin/pond CONTRACT: 1 = placed pond — S5d digs its parabolic
    // dish; 0 = natural basin — the shore-distance carve owns it. Each
    // finalize path explicitly skips the other's kind.
    u8 dug { 0 };
};

struct RiverPoint {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 surface { 0.0f };   // water level (routing surface, monotone down)
    f32 halfWidth { 0.0f }; // meters, grows with drainage area
};

struct River {
    vector<RiverPoint> points; // downstream order
};

struct HydrologyParams {
    f32 seaLevel { kDefaultSeaLevel };
    f32 minSlope { 1.0e-4f };
    // A depression counts as a lake once it is deep and wide enough —
    // below that it is just a puddle the carve pass flattens away.
    f32 minLakeDepth { 0.6f };
    u32 minLakeCells { 12 };
    f32 riverArea { 60000.0f }; // m² of drainage that starts a channel
    // halfWidth = coef * area^exponent, then narrowed by slope (see
    // pointAt) and charactered per river (smoothRiver): sqrt growth for
    // real small/large contrast, small coef to keep mountains torrent-
    // sized.
    f32 widthCoef { 0.008f };
    f32 widthExponent { 0.5f };
    u32 minRiverPoints { 6 };
    // Ponds smooth over the two spots ribbon geometry handles badly:
    // confluences (overlapping ribbons at slightly different levels) and
    // hairpin turns (the strip folds over itself). A small dug basin
    // with a flat surface absorbs both (dig depth: see FinalizeParams).
    f32 hairpinTurn { 1.9f }; // radians of turn within the window
};

struct HydrologyResult {
    vector<f32> filled;   // routing surface (priority flood)
    vector<f32> area;     // flow accumulation, m²
    vector<f32> lakeDepth; // filled - height where a lake sits, else 0
    vector<Lake> lakes;
    vector<River> rivers;
};

HydrologyResult extractHydrology(const GridSpec& spec,
                                 const vector<f32>& height,
                                 const HydrologyParams& params);

// Lakes only: connected flooded components of `height` under its
// priority-flood surface `filled`. The canonical-basin path re-floods a
// WIDER window with this — no river tracing to pay. `lakeDepthOut`, if
// given, receives the per-cell flood depth grid.
vector<Lake> extractLakes(const GridSpec& spec, const vector<f32>& height,
                          const vector<f32>& filled,
                          const HydrologyParams& params,
                          vector<f32>* lakeDepthOut = nullptr);

} // namespace render::terraingen
