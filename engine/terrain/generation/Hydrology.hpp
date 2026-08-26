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
    // Water tier (classifyRivers): 0 ruisseau (wadeable everywhere),
    // 1 rivière (entrenched; crossable at the deterministic fords),
    // 2 fleuve (the obstacle tier — promoted by MASTER-NETWORK course
    // match, so its width rides the TRUE drainage area the tile window
    // truncates). 0 = legacy for every caller that never classifies.
    u8 tier { 0 };
    // Ford spots on tier-1 rivers (world XZ): a jittered world grid
    // anchors them (tile-independent by construction), the carve caps
    // its depth there — a ford is TERRAIN, not a gameplay marker.
    vector<Vec2> fords;
    // Max drainage area seen along the trace (m²) — classification
    // input, transient (not serialized; 0 on cache reload).
    f32 mouthArea { 0.0f };
    // Head sits at a lake outlet: the course RESUMES below a lake and
    // skips the spring taper (a hairline restart read as yet another
    // interruption). Bake-time only, not serialized.
    u8 lakeFed { 0 };
};

struct HydrologyParams {
    f32 seaLevel { kDefaultSeaLevel };
    f32 minSlope { 1.0e-4f };
    // A depression counts as a lake once it is deep and wide enough —
    // below that it is just a puddle the carve pass flattens away.
    f32 minLakeDepth { 0.6f };
    u32 minLakeCells { 12 };
    // m² of drainage that starts a channel. Paces the STREAM rhythm:
    // ~one watercourse per km of walk (dev arbitration).
    f32 riverArea { 150000.0f };
    // Tier thresholds (classifyRivers): drainage area that makes a
    // rivière; the fleuve tier comes from the master network instead
    // (MasterNetworkParams::fleuveArea — true areas, not window ones).
    f32 riviereArea { 2.0e6f };
    // The fleuve reads GRAND, not merely big: its widths (floor and
    // local alike) scale by this on top of the area law — a fleuve
    // spreads in its flat valley where a rivière of the same discharge
    // stays channeled.
    f32 fleuveWidthScale { 1.6f };
    // Fords on rivières: a jittered world grid of candidate spots
    // (spacing = the guaranteed rhythm, dev: ~2 km), adopted where the
    // course passes within reach.
    f32 fordSpacing { 2000.0f };
    f32 fordReach { 700.0f };
    // Surface reprofiling (smoothRiver): the raw surface is a priority-
    // flood level — flat across every filled stretch, which read as
    // ponds. The gradient ratchet makes the water DESCEND through them,
    // never digging more than maxDrop below the flood level (no
    // artificial gorge across a plain). Lower-only: monotonicity holds.
    f32 riverMinGradient { 0.002f };
    f32 riverReprofileMaxDrop { 3.0f };
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

// Tier classification + fords (S4 conditioning tail, called by the tile
// bake — plain extractHydrology callers keep tier 0 everywhere):
// rivière by drainage area, fleuve by master-network course match (the
// TRUE areas set its width floor, monotone downstream), then the ford
// grid on the rivières. `master` may be empty (no fleuve promotion).
struct MasterRiver;
void classifyRivers(vector<River>& rivers, const HydrologyParams& params,
                    u32 seed, const vector<MasterRiver>& master);

} // namespace render::terraingen
