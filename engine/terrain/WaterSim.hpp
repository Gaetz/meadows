#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/WaterInfoMap.hpp" // HeightFn, kWaterInfoDry
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
};

// Live simulation state. spec.origin is snapped to whole texels.
struct WaterSimState {
    terraingen::GridSpec spec;
    vector<f32> terrain;
    vector<f32> depth;
    vector<f32> fE, fW, fS, fN; // outflow pipes (+x, -x, +z, -z)
    vector<f32> headBuf;        // scratch
    vector<f32> scratch;

    bool valid() const {
        return spec.n >= 8 && terrain.size() == spec.cells() &&
               depth.size() == spec.cells();
    }
};

// Immutable view for the render upload and the gameplay queries.
struct WaterSimSnapshot {
    terraingen::GridSpec spec;
    u32 marginCells { 0 };
    vector<f32> surface; // absolute level; kWaterInfoDry where dry
    vector<f32> depth;   // 0 where dry
    vector<f32> velX;    // m/s, depth-averaged
    vector<f32> velZ;
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

// Shift the window by whole cells (positive = origin moves +x/+z).
// Interior content moves bit-exactly (memmove); entered strips get
// terrain from `height`, depth from an outward sweep bounded by the
// surviving edge surface (lakes arrive full), and the adjacent edge's
// pipe fluxes (through-flowing rivers arrive MOVING, not stalled).
void scrollWindow(WaterSimState& state, i32 dCol, i32 dRow,
                  const HeightFn& height, f32 seaLevel);

// Build the immutable snapshot (dry threshold + velocity derivation
// live here, not in the state).
void extractSnapshot(const WaterSimState& state,
                     const WaterSimParams& params,
                     WaterSimSnapshot& out);

// Async spin-up for init/teleport: the offline multigrid solver runs
// to (approximate) equilibrium and seeds the state — the window opens
// on settled water instead of minutes of filling. Pipes start at rest
// and rebuild momentum in a few seconds (hidden by the crossfade).
WaterSimState preRollWindow(const terraingen::GridSpec& spec,
                            const HeightFn& height,
                            const WaterSimParams& params,
                            const vector<terraingen::WaterSource>& sources);

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

} // namespace render::terrain
