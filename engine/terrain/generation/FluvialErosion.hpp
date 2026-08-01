#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Stage S2 — fluvial erosion: tectonic uplift + stream-power incision,
// solved with the IMPLICIT scheme of Braun & Willett 2013 ("fastscape",
// the engine under Cordonnier et al. EG 2016). Unconditionally stable at
// large dt, O(n log n) per iteration, and the input is an uplift map —
// which is exactly what the control layer paints/derives. This is what
// turns noise bumps into dendritic ridge-and-valley macro structure.

namespace render::terraingen {

struct FluvialParams {
    i32 iterations { 100 };
    f32 dt { 1.0f };
    // Stream-power erodibility: dh/dt = -k * A^m * slope, with A in m².
    // Sets the equilibrium slope S = upliftRate / (k * A^m): at A = 1e4 m²
    // these values give S ~ 1.0 (alpine walls), S ~ 0.3 in the big
    // valleys (A ~ 1e5) — real mountains, walkable floors.
    f32 k { 8.0e-2f };
    f32 areaExponent { 0.5f }; // m of A^m
    // Meters of rock raised per iteration where the uplift field is 1:
    // x iterations = the orogeny budget the erosion carves into
    // (~800 m over the macro base at the defaults).
    f32 upliftRate { 8.0f };
    // Base level: nodes at/below the sea (and the grid rim) are fixed —
    // erosion carves toward them and never below.
    f32 seaLevel { 21.0f };
    // Epsilon slope used by the depression routing so flats drain.
    f32 minSlope { 1.0e-4f };
    // The flood + receiver routing (the expensive O(n log n) part) is
    // recomputed every this many iterations — heights move slowly per
    // step, reusing the drainage tree in between is visually free and
    // ~3x faster end to end.
    i32 routingInterval { 4 };
};

struct FluvialResult {
    vector<f32> height;
    // Final drainage area per texel (m²) on the depression-routed
    // surface — stage S4 extracts rivers from it for free.
    vector<f32> area;
};

// `height`/`uplift` are spec-sized grids (S1 output). `keep`, if given,
// re-blends each texel toward its INPUT height by [0,1] — how mesa tiers
// survive full stream-power dissection (plateauKeep). `erodibility`, if
// given, scales k per texel (biome character: hard rock vs sediment).
FluvialResult erodeFluvial(const GridSpec& spec, const vector<f32>& height,
                           const vector<f32>& uplift,
                           const FluvialParams& params,
                           const vector<f32>* keep = nullptr,
                           const vector<f32>* erodibility = nullptr);

// Priority-flood depression fill (Barnes et al. 2014, epsilon variant):
// returns the water-routing surface >= height where every node drains to
// a base-level node (grid rim or height <= seaLevel). Shared by S2
// (receiver routing) and S4 (lakes = filled - height).
vector<f32> priorityFloodFill(const GridSpec& spec,
                              const vector<f32>& height, f32 seaLevel,
                              f32 minSlope);

// Steepest-descent flow routing over an already-drained surface (the
// priority-flood fill): receiver graph, ascending process order and
// accumulated drainage area (m²). The shared core of S2's iteration and
// S4's river extraction. `trueHeight` decides base level (sea/rim);
// receivers/order follow `routed`.
struct FlowRouting {
    vector<u32> receiver; // self = base level
    vector<f32> recvDist; // meters to the receiver
    vector<u32> order;    // indices, routed surface ascending
    vector<f32> area;     // m², donors accumulated into receivers
};
FlowRouting routeFlow(const GridSpec& spec, const vector<f32>& routed,
                      const vector<f32>& trueHeight, f32 seaLevel);

} // namespace render::terraingen
