#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/WaterBodies.hpp" // LakeSurface, HeightFn
#include "engine/terrain/generation/WaterSolve.hpp"

// Real-time windowed shallow-water simulation (docs/WATER-RESEARCH.md,
// option C — the From Dust model, dev decision 2026-08-27): the SAME
// virtual-pipes kernel as the offline solver, but stepped continuously
// on a camera-following window laid on the RENDER-resolution terrain —
// water hugs the ground by construction, falls descend cliffs cell by
// cell, and terrain edits react live. Headless and doctested; the
// offline solveSteadyWater stays the equilibrium oracle (cross-checked)
// and the pre-roll engine. State is transient by design: never
// serialized as truth (dev arbitration — leaving an area re-simulates
// it from the persisted terrain).
//
// Threading contract: a WaterSimState is owned by exactly ONE thread at
// a time (the step job while in flight, main otherwise); consumers read
// immutable WaterSimSnapshots, never the live arrays.

namespace render::terrain {

struct WaterSimParams {
    // 30 Hz real-time tick. CFL: the pipe scheme's wave speed is
    // c = sqrt(g * texel) (depth-independent), so dt scales with
    // sqrt(texel); at 2 m the offline-validated Courant point allows
    // dt ~0.05 — 1/30 is 1.5x safer. Pre-roll/burst may use 0.05.
    f32 dt { 1.0f / 30.0f };
    f32 gravity { 9.81f };
    f32 friction { 0.995f }; // per substep — the settling knob
    f32 rainRate { 4.0e-6f };        // m/s, the judged demo storm
    f32 evaporationRate { 3.0e-6f }; // strictly < rain (offline lesson)
    f32 seaLevel { kDefaultSeaLevel };
    // Open borders, expressed PER SECOND (the offline 0.05/iter at
    // dt 0.1 is keep 0.6/s); converted per substep inside the kernel.
    f32 borderDrainPerSecond { 0.4f };
    // Applied at snapshot EXTRACTION only — the live state keeps its
    // thin films (drying in-state oscillates wet/dry every step).
    f32 dryThreshold { 0.02f };
    // Trusted inset (cells): rendering and queries stay this far from
    // the window border; entering strips settle inside the margin.
    u32 marginCells { 32 };
    // Max OUTFLOW of a pinned reservoir cell (m³/s) — its weir rate.
    // The pin holds the lake level (instant refill: bounded refill
    // collapsed the surface into a meters-deep drawdown cone), so the
    // supply must be bounded on the way OUT: unbounded, the rim over a
    // carved outlet poured ~6,200 m³/s (replay-measured, Amazon-grade)
    // and flooded the gorge until water climbed the far slopes. Only
    // cells with a real head difference leak at all — this caps each.
    // (Appended LAST: WSD1 dumps read as a prefix of this struct.)
    f32 reservoirOutflow { 2.0f };
};

// Live simulation state. spec.origin is snapped to whole texels.
struct WaterSimState {
    terraingen::GridSpec spec;
    vector<f32> terrain;
    vector<f32> depth;
    vector<f32> fE, fW, fS, fN; // outflow pipes (+x, -x, +z, -z)
    vector<f32> headBuf;        // scratch
    vector<f32> scratch;
    // Pinned reservoirs (kWaterInfoDry = free cell): the surface is
    // HELD at this level every substep, absorbing and supplying like
    // the sea — baked lakes become infinite reservoirs, so a lake cut
    // by (or bordering) the window keeps its authored level and POURS
    // over every rim cell below it: the wide waterfall the flat lake
    // sheet only implied. Breaching a rim by sculpting drains forever
    // (the reservoir never empties) — accepted, and very From Dust.
    vector<f32> pinned;
    // Persistent RENDER wetness with hysteresis (updated at
    // extraction): cells near the publish threshold flickered wet/dry
    // per tick, blinking whole surfaces out and popping orphan walls
    // at their neighbours (docs/WATER-RENDER.md §1.3).
    vector<u8> wetMask;
    // Brook footprints (E4b): cells under a baked ribbon too narrow
    // to pin. Extraction publishes them at reduced thresholds so a
    // tier-0 film shows in its carved bed. Rebuilt by pinRivers on
    // every init/scroll (never crumb-copied).
    vector<u8> brookMask;

    bool valid() const {
        return spec.n >= 8 && terrain.size() == spec.cells() &&
               depth.size() == spec.cells();
    }
};

// Immutable view for the render upload and the gameplay queries.
// Sea-pinned cells (ground under sea, level at sea) read DRY here: the
// analytic ocean sheet renders them and the sea query path answers for
// them — the sim window never fights either.
struct WaterSimSnapshot {
    terraingen::GridSpec spec;
    u32 marginCells { 0 };
    vector<f32> surface; // absolute level; kWaterInfoDry where dry
    vector<f32> depth;   // 0 where dry
    vector<f32> velX;    // m/s, depth-averaged
    vector<f32> velZ;
    // Render displacement plane: the wet surface, or the local ground
    // minus a tuck where dry (kept for texture-side consumers).
    vector<f32> display;
    // The ONE render geometry (docs/WATER-RENDER.md §2): a closed
    // skin built on the worker by MARCHING SQUARES on the dual grid
    // (cell centers as samples) — tops cover the wet-region polygon
    // per 2x2 block, one column-capped wall per contour segment; the
    // shoreline is a 45°-chamfered polyline refined sub-texel by
    // depth interpolation. Vertex = pos3 + uv2 (sim-texture uv).
    vector<f32> meshVerts;
    vector<u32> meshIndices;
};

// Fresh window: terrain from `height` per cell, depth from the
// priority-flood surface (lakes start full), pipes at rest, sea pinned.
void initWindow(WaterSimState& state, const terraingen::GridSpec& spec,
                const HeightFn& height, f32 seaLevel);

// Advance the window by `substeps` kernel steps (pipes -> mass limiter
// -> divergence -> open borders -> sea pin -> sources). Deterministic:
// fixed iteration order, single-threaded.
void stepWindow(WaterSimState& state, const WaterSimParams& params,
                const vector<terraingen::WaterSource>& sources,
                u32 substeps);

// Re-sample the whole terrain plane (terraforming: the sculpt overlay
// changed under the window — the water then reacts live).
void refreshTerrain(WaterSimState& state, const HeightFn& height);

// Rebuild the pinned-reservoir plane from the baked lakes AND seed
// their full footprint (call after init, scroll or terrain refresh;
// the whole plane is cheap). Seeding fills every (near-dry) covered
// cell once to the baked level — the lake occupies its whole baked
// shape immediately; the PIN itself only holds the mask interior
// (eroded one mask texel — overhang pins were artesian springs).
void pinLakes(WaterSimState& state, const WaterBodies& bodies);

// The river-tier extension of the pins (E1b): ribbons at or above
// `minHalfWidth` PLACE their water — their carved-bed cells are
// seeded AND pinned to the baked ribbon surface (reconciled against
// the final ground at bake), so a big river no longer "disappears"
// under the sim window while the sim re-derives it from entry
// sources. Guards inherited from the lakes: only ground clearly
// BELOW the surface is pinned (banks never — the artesian lesson),
// the pin core is eroded one texel inside the ribbon half-width, and
// the reservoir weir cap bounds the outflow. Small rivers stay 100%
// sim. Call AFTER pinLakes (which resets the pinned plane).
void pinRivers(WaterSimState& state, const WaterBodies& bodies,
               f32 minHalfWidth);

// Shift the window by whole cells (positive = origin moves +x/+z).
// Interior content moves bit-exactly (memmove); entered strips get
// terrain from `height` and start DRY (the sim moves water, it never
// invents it) — UNLESS a breadcrumb covers the cell: `crumbs` are
// previously SIMULATED window states (the session cache) and their
// water is remembered, not invented — depth, pipes and wet memory are
// copied over, so walking back to a waterfall finds it flowing.
void scrollWindow(
    WaterSimState& state, i32 dCol, i32 dRow, const HeightFn& height,
    f32 seaLevel,
    const vector<sptr<const WaterSimState>>* crumbs = nullptr);

// Build the immutable snapshot AND its render mesh (dry threshold,
// hysteresis, velocity derivation and geometry all live here, not in
// the kernel). Mutates only state.wetMask.
void extractSnapshot(WaterSimState& state, const WaterSimParams& params,
                     WaterSimSnapshot& out);

// Async spin-up for init/teleport: the offline multigrid solver runs
// to (approximate) equilibrium and seeds the state — the window opens
// on settled water instead of minutes of filling. Pipes start at rest
// and rebuild momentum in a few seconds (hidden by the crossfade).
WaterSimState preRollWindow(const terraingen::GridSpec& spec,
                            const HeightFn& height,
                            const WaterSimParams& params,
                            const vector<terraingen::WaterSource>& sources);

// Session LRU of evicted window states (the re-entry lever,
// docs/WATER-RENDER.md §4): pick the cached window that best overlaps
// the target, so the runtime resumes it via scrollWindow instead of
// re-running the pre-roll solver — returning water is "already
// flowing". Pure and headless; the cache itself lives with the
// renderer (main thread) and is NEVER serialized (§2.4: a save is a
// patch layer, not sim state).
struct CachedWindowPick {
    i32 index { -1 }; // -1 = no usable candidate
    i32 dCol { 0 };   // scrollWindow shift from the cached origin
    i32 dRow { 0 };
};
// Candidates must match the target's texel and n exactly (a knob
// change makes old states unusable); `minOverlap` is the kept-area
// fraction below which a fresh pre-roll beats scrolling mostly-dry
// strips.
CachedWindowPick chooseCachedWindow(
    const vector<terraingen::GridSpec>& cached,
    const terraingen::GridSpec& target, f32 minOverlap = 0.25f);

// Bilinear sample of a snapshot at a world position; wet-weighted like
// the region sampler (a dry corner never drags the level down at the
// bank). Returns depth 0 outside the trusted rect.
struct WaterSimSample {
    f32 depth { 0.0f };
    f32 surface { 0.0f };
    f32 velocityX { 0.0f };
    f32 velocityZ { 0.0f };
};
WaterSimSample sampleSnapshot(const WaterSimSnapshot& snap, f32 x,
                              f32 z);

// On-site debugging (dev request): dump the LIVE window (all planes +
// params + sources) to a binary file, reload it offline and replay it
// deterministically — `cooker water-replay` renders judgment maps from
// these. The dump is a diagnostic artifact, never a save format.
bool dumpSimState(const WaterSimState& state,
                  const WaterSimParams& params,
                  const vector<terraingen::WaterSource>& sources,
                  const char* path);
bool loadSimState(const char* path, WaterSimState& state,
                  WaterSimParams& params,
                  vector<terraingen::WaterSource>& sources);

} // namespace render::terrain
