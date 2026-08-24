#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Top-down overview maps of the ANALYTIC sandbox macro (the far-terrain
// surface — no tile bakes, so any span renders in seconds): hypsometric
// tints + hillshade + the master-network river courses. Consumers: the
// `cooker terrain-map` subcommand (PNG), diagnostics, later the editor.
// Pure function of its inputs — no file IO here (the tool owns the PNG
// encoder).

namespace render::terraingen {

struct TerrainMapParams {
    f32 centerX { 0.0f };
    f32 centerZ { 0.0f };
    f32 span { 10000.0f }; // world meters covered by the image width
    u32 size { 1000 };     // pixels per side
    bool drawRivers { true }; // master-network fleuve courses
    bool markCenter { true }; // small cross at the center
};

// RGB8, size*size*3, row 0 = north (centerZ - span/2).
vector<u8> renderTerrainMap(const ProceduralControls& controls,
                            const MacroParams& macro,
                            const TerrainMapParams& params);

} // namespace render::terraingen
