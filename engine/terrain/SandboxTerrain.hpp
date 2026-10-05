#pragma once

#include "engine/terrain/generation/TerrainGen.hpp"

// Generated-world identity ("sandbox" = generated, as opposed to the
// story demo noise): the seed-derived control/macro parameters, the map
// lattice and the active map's overview. When TerrainParams.sandbox is
// set, the procedural fallback OUTSIDE published slices becomes the
// map overview where it exists, else the analytic S1 macro
// (macroHeightAnalytic) — so FarTerrain silhouettes, unbaked maps and
// not-yet-streamed ground agree with what the bake produces. Null = the
// legacy demo noise (the story world stays bit-identical).

namespace render {

struct SandboxTerrain {
    terraingen::ProceduralControlParams controls;
    terraingen::MacroParams macro;
    // Bounded-map border transitions (chantier CARTES v2): the
    // analytic fallback applies the same border-line shaping the bakes
    // use — one pure function of (seed, lattice), so the horizon,
    // every unbaked map and the baked rims agree by construction.
    terraingen::MapGridSpec grid;
    // The baked map's 64 m OVERVIEW (decimated global stage-1, rim
    // included): the fallback INSIDE its coverage — a pointwise
    // analytic mirror cannot follow a globally carved valley network
    // (measured hundreds of meters of drift); the map's own coarse
    // truth can. Empty = analytic fallback (legacy / unbaked map).
    terraingen::GridSpec overviewGrid;
    vector<f32> overview;
};

} // namespace render
