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
    // Bounded-map rim (chantier CARTES M3.1): when valid, the analytic
    // fallback OUTSIDE baked slices applies the same edge shaping the
    // map bake used — sea sides read as endless ocean, ridge sides as
    // the rim crest decaying outward, matching the baked rim at the
    // map line by construction.
    terraingen::MapEdgeSpec edge;
    // The baked map's 64 m OVERVIEW (decimated global stage-1, rim
    // included): the fallback INSIDE its coverage — a pointwise
    // analytic mirror cannot follow a globally carved valley network
    // (measured hundreds of meters of drift); the map's own coarse
    // truth can. Empty = analytic fallback (legacy / unbaked map).
    terraingen::GridSpec overviewGrid;
    vector<f32> overview;
};

} // namespace render
