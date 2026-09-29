#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/WaterSolve.hpp" // terraingen::GridSpec

// Chantier ESPRITS E3 — the fire field: a camera window of 2 m cells
// (the water sim's footprint) where heat spreads over fuel. The Far Cry 2
// model (docs/CHANTIER-ESPRITS.md §2): a burning cell damages its eight
// neighbours by spreadRate x ((1 - 0.75·w) + 0.75·w·max(0, wind·dir)) x
// their flammability x (1 - moisture), w = the wind's strength; a cell ignites once its heat reaches
// ignitionPoints, at most spreadBudgetPerTick ignitions per tick (never
// "the whole map burns"); burning consumes fuel, spent fuel is BURNT;
// water under a cell puts it out and keeps it out (the BotW rule
// Water > Fire, the triad shifumi). Fuel is sampled LAZILY through FuelFn
// the first time a cell matters, so a window costs nothing until fire
// touches it. Headless and deterministic: the same inputs give the same
// grid bit for bit (the job path relies on it).

namespace render::terrain {

struct FireCellFuel {
    f32 fuel { 0.0f };         // seconds of burning the cell holds
    f32 flammability { 0.0f }; // 0..1 how readily heat takes
    f32 moisture { 0.0f };     // 0..1 damps the spread
};
using FuelFn = std::function<FireCellFuel(f32 x, f32 z)>;
using WetFn = std::function<bool(f32 x, f32 z)>; // standing water there
// The wind over a cell (unit-ish direction x strength 0..1); null = the
// uniform FireParams::wind.
using WindFn = std::function<Vec2(f32 x, f32 z)>;

enum class FireState : u8 { Dormant = 0, Burning = 1, Burnt = 2, Wet = 3 };

struct FireGrid {
    terraingen::GridSpec spec;
    vector<f32> heat;    // accumulated ignition damage
    vector<f32> fuel;    // remaining; < 0 = not sampled yet
    vector<f32> fuel0;   // as sampled (scorch = 1 - fuel/fuel0)
    vector<f32> flammability;
    vector<f32> moisture;
    vector<u8> state;    // FireState
    vector<f32> ember;   // 1 at burnout, cooling to 0 over emberSeconds
    vector<f32> regrow;  // burnt: 0..1 back to life (fuel and green return)
    u64 tick { 0 };
    size_t cells() const { return spec.cells(); }
    bool valid() const { return spec.n > 0 && heat.size() == cells(); }
};

struct FireParams {
    f32 dt { 1.0f / 10.0f };       // the fire ticks at 10 Hz
    f32 spreadRate { 1.0f };       // heat/s a burning cell deals a neighbour
    f32 ignitionPoints { 1.0f };   // heat a cell takes before it burns
    u32 spreadBudgetPerTick { 64 };
    f32 burnRate { 1.0f };         // fuel/s consumed while burning
    f32 heatDecay { 0.5f };        // heat/s lost by a cell not burning
    // The FRONT's width in time: a cell glows (and carries flames) for
    // this long after ignition, then burns on darkly; a burnt cell's
    // embers cool over the same span. At the front's speed this is its
    // width in metres.
    f32 emberSeconds { 45.0f };
    // The burnt state is TRANSITORY: a burnt cell regrows over this many
    // seconds on dry ground, faster the wetter its ground (x (1 + 3 x
    // moisture)); regrown, it is dormant again with its fuel restored,
    // and its char fades back to green meanwhile.
    f32 regrowSeconds { 180.0f };
    Vec2 wind { 0.0f, 0.0f };      // unit-ish direction x strength (0..1)
};

struct FireStats {
    u32 burning { 0 };
    u32 ignited { 0 };   // this tick
    u32 doused { 0 };    // put out by water this tick
    u32 burnt { 0 };     // total burnt cells
};

// A fresh window: nothing sampled, nothing burning.
void fireInitWindow(FireGrid& grid, const terraingen::GridSpec& spec);
// Scrolls the window by whole cells (the camera moved): the interior
// shifts bit-exactly, entering cells start fresh.
void fireScrollWindow(FireGrid& grid, i32 dCol, i32 dRow);

// A source: `heat` dealt to every cell within `radius` metres of (x, z)
// this tick (a spell, an ember). Sampling fuel where needed.
void fireIgnite(FireGrid& grid, f32 x, f32 z, f32 radius, f32 heat,
                const FuelFn& fuel);

// The opposite of a spark: every cell within `radius` metres of (x, z)
// stops burning (its fuel stays — it can catch again) and loses its heat
// and embers; burnt ground stays burnt.
void fireDouse(FireGrid& grid, f32 x, f32 z, f32 radius);

// One tick. Order per cell is fixed (row-major), so two identical grids
// stepped with identical inputs stay identical.
void fireStep(FireGrid& grid, const FireParams& params, const FuelFn& fuel,
              const WetFn& wet, FireStats* stats = nullptr,
              const WindFn& windAt = {});

// The render mask: 0 untouched .. 1 charred. A cell chars as the front
// passes over it (over emberSeconds from its ignition), whatever fuel
// it still burns through darkly afterwards, and fades back as it
// regrows (regrowSeconds). One byte per cell.
void fireScorch(const FireGrid& grid, const FireParams& params, vector<u8>& out);
// The ember mask: how brightly a cell glows — a burning cell fading
// from ignition over emberSeconds, a burnt cell by its cooling embers,
// a dormant cell warming toward ignition faintly, wet / cold cells not
// at all. One byte per cell.
void fireGlow(const FireGrid& grid, const FireParams& params, vector<u8>& out);

// The cells burning right now, freshest first, at most `maxCount` — the
// emitter budget (world XZ centers). `maxBurnedSeconds` keeps only the
// cells ignited that recently (the front; infinity = every burning
// cell). Cells heating past half their ignition point count too (last
// in the order): the flames reach the front's leading edge, not a cell
// behind it.
vector<Vec2> fireBurningCenters(const FireGrid& grid, u32 maxCount,
                                const FireParams& params = FireParams {},
                                f32 maxBurnedSeconds = 1.0e9f);

} // namespace render::terrain
