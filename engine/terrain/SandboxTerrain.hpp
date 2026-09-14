#pragma once

#include "engine/terrain/generation/TerrainGen.hpp"

// Sandbox world identity: the seed-derived control/macro parameters.
// When TerrainParams.sandbox is set, the procedural fallback OUTSIDE
// baked tiles becomes the analytic S1 macro (macroHeightAnalytic), so
// FarTerrain silhouettes and not-yet-baked ground agree with the tiles
// the streamer will bake there. Null = the legacy demo noise (the
// existing world stays bit-identical).

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
