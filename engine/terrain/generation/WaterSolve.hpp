#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Steady-state water solve (docs/WATER-RESEARCH.md, option D): the
// virtual-pipes shallow-water model run OFFLINE to equilibrium — rain
// falls on every cell, flows downhill through per-cell pipes, and
// drains into the sea; what remains is a physically coherent water
// body per texel: DEPTH (rivers hold their equilibrium level, sloped
// through rapids, flat in pools) and VELOCITY (the current). Pure and
// deterministic (fixed iteration order, no threads inside): the same
// inputs give the same fields bit for bit — bake-cacheable like every
// other stage.
//
// Real-time variant deliberately NOT built (dev arbitration): the same
// solver windowed around the camera would enable dynamic water; kept
// as a documented option for future water-creation spells.

namespace render::terraingen {

struct WaterSolveParams {
    // Rain in meters PER SECOND (applied x dt per iteration) — the
    // stylized runoff knob (1e-5 m/s = a 36 mm/h permanent storm:
    // real-world runoff at our compressed basin sizes would make
    // creeks of every fleuve; tune for game-scale discharges).
    f32 rainRate { 1.0e-5f };
    // Evaporation, m/s, STRICTLY below rain (net runoff must stay
    // positive or nothing ever flows — measured: evap > rain froze the
    // whole grid at 0.06 m/s). Pans still fill to their spill and
    // become lakes: that is the honest steady state; size filtering
    // belongs to the consumer, not the solver.
    f32 evaporationRate { 3.0e-6f };
    f32 dt { 0.1f };           // pipe integration step
    f32 gravity { 9.81f };
    f32 friction { 0.995f };   // flux damping per iteration (momentum
                               // must build up for gentle-slope flux)
    // Start from the priority-flood surface: lakes begin FULL (filling
    // a deep basin by rain took tens of thousands of iterations and
    // dominated the residual) — only the channel dynamics settle.
    bool warmStart { true };
    f32 seaLevel { kDefaultSeaLevel };
    u32 maxIterations { 24000 };
    // Convergence: solved when the max |depth change| over a check
    // window drops under eps (meters).
    f32 convergenceEps { 1.0e-3f };
    u32 checkInterval { 250 };
    // Depths under this are dried at the end (film noise, not water).
    f32 dryThreshold { 0.02f };
    // Multigrid: solve coarse levels first (min-downsampled terrain —
    // channels survive the downsample) and transfer the SURFACE as the
    // next level's warm start. The equilibrium is a large-scale
    // phenomenon: the fine level only pays local correction — measured
    // as the difference between a 124 s and a usable bake budget.
    bool multigrid { true };
    u32 multigridMinN { 96 };      // stop coarsening below this grid
    u32 fineIterations { 3000 };   // per refined level (coarse levels
                                   // are capped separately)
};

// A point discharge injected every iteration (m³/s) — the boundary
// inflow: a course entering the solve window carries its whole
// upstream catchment, which the window cannot see (the master
// network's TRUE areas provide it; a wall border without sources
// starves every through-flowing river — measured).
struct WaterSource {
    f32 x { 0.0f };
    f32 z { 0.0f };
    f32 discharge { 0.0f };
};

struct WaterSolveResult {
    GridSpec spec;
    vector<f32> depth;     // m of standing water per texel (0 = dry)
    vector<f32> velocityX; // m/s, depth-averaged
    vector<f32> velocityZ;
    // Through-discharge per texel (m³/s): THE river-trace signal. Mass
    // conservation makes the high-flux paths CONTINUOUS from source to
    // sea, even where the water film is centimeters thin on a steep
    // stretch — a course is where the flux runs, not where the depth
    // pools (the depth-thresholded view broke rivers into puddles,
    // measured on the judgment maps).
    vector<f32> flux;
    u32 iterations { 0 };
    f32 residual { 0.0f }; // last max |depth change| observed
};

// `terrain` is spec.cells() ground heights. Sea cells (ground under
// seaLevel) are held at sea surface — they absorb and supply; the grid
// border is a wall (size the spec so the margin holds the basin, and
// feed through-flowing courses with `sources`).
WaterSolveResult solveSteadyWater(const GridSpec& spec,
                                  const vector<f32>& terrain,
                                  const WaterSolveParams& params,
                                  const vector<WaterSource>* sources =
                                      nullptr);

} // namespace render::terraingen
