#pragma once

#include <cmath>

#include <glm/glm.hpp>

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

// The default width law, halfWidth = kRiverWidthCoef * sqrt(area) —
// ONE home for the coefficient (it was duplicated as a literal in
// every runtime consumer; retuning it must retune them all). The
// helpers assume the default sqrt exponent.
constexpr f32 kRiverWidthCoef = 0.008f;
inline f32 riverHalfWidthFromArea(f32 area) {
    return kRiverWidthCoef * std::sqrt(glm::max(area, 0.0f));
}
inline f32 riverAreaFromHalfWidth(f32 halfWidth) {
    const f32 r = glm::max(halfWidth, 0.0f) / kRiverWidthCoef;
    return r * r;
}

// The ONE width law of the master courses (passe EAU E4b), shared by
// the imprint, the tier classification and the far-water fallback:
// half = min(coef·q^exp, maxHalf), scaled toward fleuveScale on a
// smooth discharge ramp. NO hard floor any more — a head is born at
// its own gabarit (~5 m half at the extraction threshold) and grows at
// every confluence; the old 12 m floor made every fleuve spring out of
// the ground 38 m wide.
struct RiverWidthLaw {
    f32 coef { kRiverWidthCoef };
    f32 exponent { 0.5f };
    f32 maxHalf { 45.0f };
    f32 fleuveScale { 1.6f };
    f32 rampLow { 4.0e6f };  // m² of qArea where the GRAND read begins
    f32 rampHigh { 1.0e7f }; // ... and where it is full
};
inline f32 riverLawHalfWidth(const RiverWidthLaw& law, f32 q) {
    const f32 base = glm::clamp(
        law.coef * std::pow(glm::max(q, 0.0f), law.exponent), 0.0f,
        law.maxHalf);
    f32 t = glm::clamp(
        (q - law.rampLow) / glm::max(law.rampHigh - law.rampLow, 1.0f),
        0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return base * glm::mix(1.0f, law.fleuveScale, t);
}

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
    // m² of effective drainage feeding the lake (max accumulation over
    // its basin — the spill carries it all): the outlet river's width
    // justification, and the E7 report's size measure. TWB5.
    f32 catchment { 0.0f };
};

struct RiverPoint {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 surface { 0.0f };   // water level (routing surface, monotone down)
    f32 halfWidth { 0.0f }; // meters, grows with drainage area
    // Water tier OF THE POINT (E4b): one course is a wadeable rivière
    // in its mountains and the obstacle fleuve in its plain; monotone
    // downstream by construction (the discharge only grows). u32 so
    // the serialized blob carries no padding (TWB4).
    u32 tier { 0 };
    // m² of drainage AT the point (E9): the window accumulation, or
    // the master qArea where a node matched — the runtime's discharge
    // truth (the old width->area inverse undid only the bare sqrt law
    // and lied on every narrowed, floored or ramped reach). TWB6.
    f32 area { 0.0f };
};

// Tier character as the runtime current (m/s): the ruisseau ambles, the
// rivière flows, the fleuve DRAGS (the swim controller multiplies by
// swimDriftFactor — the fleuve is the obstacle tier). ONE law for the
// baked sandbox rivers and the exported RiverForm records.
inline f32 riverFlowSpeedForTier(u8 tier) {
    return tier == 0 ? 0.6f : tier == 2 ? 1.8f : 1.0f;
}

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
    // input, serialized since TWB4 (a cache reload reclassifies the
    // same way the bake did).
    f32 mouthArea { 0.0f };
    // Head sits at a lake outlet: the course RESUMES below a lake and
    // skips the spring taper (a hairline restart read as yet another
    // interruption). Serialized since TWB4.
    u8 lakeFed { 0 };
    // E5: index of the lake this course springs from (-1 = none).
    i32 sourceLake { -1 };
    // E6: source typology of the head (spring/névé/glacier/lake).
    u32 headType { 0 };
    // This RUN starts at the course's TRUE head (not a slice cut or a
    // lake resume): the runtime dresses sources off it. TWB7.
    u8 headIsSource { 0 };
};

struct HydrologyParams {
    f32 seaLevel { kDefaultSeaLevel };
    f32 minSlope { 1.0e-4f };
    // A depression counts as a lake once it is deep and wide enough —
    // below that it is just a puddle the carve pass flattens away.
    // World units (m²), so the hydrology window texel is a free knob:
    // written as the legacy cell count times the 16 m cell area.
    f32 minLakeDepth { 0.6f };
    f32 minLakeArea { 12.0f * 16.0f * 16.0f };
    // The outlet piercing (passe EAU E5, l'« enquête eau ») : before
    // extraction, every depression that does NOT earn its lake (big
    // enough OR deep enough) is DRAINED through a notch carved at its
    // spill — a valley again, not a filling. The keepers are the 3-6
    // destination lakes a map should hold, not the 150+ potholes the
    // erosion leaves.
    bool lakePiercing { true };
    // Retention is a BAND on the spill-lock depth (the notch a pierce
    // would cut at the col ~ the basin's depth) — a LOCAL criterion,
    // stable between neighbouring windows where the old cell-count
    // test flipped (one map kept the basin its neighbour pierced).
    // Below the band: a dip any stream re-cuts — a valley, pierced.
    // Inside: a true kettle/cirque lake — kept. At or above
    // lakeDeepPierce: no natural col is that high here — a storey
    // wall drowned a valley network; pierced as a gorge through the
    // wall (the ramp follows the steepest descent to daylight).
    f32 lakeKeepDepth { 12.0f };
    f32 lakeDeepPierce { 60.0f };
    // Pierce FLOOR: a basin both smaller and shallower than this is
    // left as a filled flat (no notch) — the fine ground's own
    // micro-relief (±1.1 m fbm, 4 m fine erosion) seeds thousands of
    // sub-width bowls on a fine routing grid, and notching them all
    // both exhausts the pass budget and engraves rills everywhere.
    // Must stay under minLakeArea, so every lake-eligible basin still
    // gets its pierce-or-keep decision.
    f32 pierceMinArea { 2048.0f };
    f32 pierceMinDepth { 2.0f };
    // Extent floor INSIDE the band: the storey-wall foot trenches are
    // 10-60 m deep but a few cells wide — depth alone kept 124 of
    // them on one map (measured). A destination lake is deep AND
    // broad; ~10 ha, in m² (texel-free).
    f32 lakeKeepArea { 400.0f * 16.0f * 16.0f };
    // Mouth flare (estuaries): a course that reaches the sea opens
    // over its last metres — half-width ramps to x mouthFlare. The
    // ribbon and the bed carve both read hw, so the bed follows.
    f32 mouthFlare { 1.8f };
    f32 mouthFlareLength { 300.0f };
    // m² of drainage that starts a channel. Paces the STREAM rhythm:
    // ~one watercourse per km of walk (dev arbitration).
    f32 riverArea { 150000.0f };
    // Tier thresholds (classifyRivers): drainage area that makes a
    // rivière; fleuveArea mirrors MasterNetworkParams::fleuveArea
    // (same pupitre knob) — the tier-2 cut of a matched point's qArea.
    f32 riviereArea { 2.0e6f };
    f32 fleuveArea { 6.0e6f };
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
    // sized. The default coef is shared with every runtime consumer
    // (kRiverWidthCoef above).
    f32 widthCoef { kRiverWidthCoef };
    f32 widthExponent { 0.5f };
    // The fleuve GRAND ramp and the obstacle guarantee (E4b): the
    // fleuveWidthScale blends in between these qAreas instead of
    // multiplying a whole course, and a tier-2 point never reads
    // narrower than tier2MinHalfWidth.
    f32 fleuveRampLow { 4.0e6f };
    f32 fleuveRampHigh { 1.0e7f };
    f32 tier2MinHalfWidth { 12.0f };
    u32 minRiverPoints { 6 };
    // Ponds smooth over the two spots ribbon geometry handles badly:
    // confluences (overlapping ribbons at slightly different levels) and
    // hairpin turns (the strip folds over itself). A small dug basin
    // with a flat surface absorbs both (dig depth: see FinalizeParams).
    f32 hairpinTurn { 1.9f }; // radians of turn within the window

    RiverWidthLaw widthLaw() const {
        return { widthCoef, widthExponent, 45.0f, fleuveWidthScale,
                 fleuveRampLow, fleuveRampHigh };
    }
};

struct HydrologyResult {
    vector<f32> filled;   // routing surface (priority flood)
    vector<f32> area;     // flow accumulation, m²
    vector<f32> lakeDepth; // filled - height where a lake sits, else 0
    vector<Lake> lakes;
    vector<River> rivers;
};

// Accumulation ALONE (flood + route, no piercing, no lakes, no
// courses): the ground pass's ravine-steering field — approximate
// drainage may shape the ground, but no WATER (levels, masks,
// courses) is ever placed from it.
vector<f32> drainageArea(const GridSpec& spec, const vector<f32>& height,
                         const HydrologyParams& params);

HydrologyResult extractHydrology(const GridSpec& spec,
                                 const vector<f32>& height,
                                 const HydrologyParams& params);

// The outlet piercing (E5, l'« enquête eau ») : flood, find the
// depressions, DRAIN every one that earns no lake (the lakeKeepDepth /
// lakeDeepPierce band) through a descending notch carved at its spill, and
// iterate (a drained bowl can reveal the next). Mutates `ground`
// BEFORE extractHydrology; bounded; no-op when lakePiercing is off.
void pierceLakeOutlets(const GridSpec& spec, vector<f32>& ground,
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
