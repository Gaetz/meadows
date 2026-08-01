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
};

} // namespace render
