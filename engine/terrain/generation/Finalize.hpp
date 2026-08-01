#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/Hydrology.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Stages S5 + S6 — finalization: bicubic upsample of the eroded macro
// grid to runtime texels, river-bed carving along the extracted
// polylines, a mask-modulated mid-frequency relief pass (so the upsample
// does not read as blurred macro), and the derived mask channels the
// splat/scatter/water systems consume.

namespace render::terraingen {

struct FinalizeParams {
    u32 upsampleFactor { 4 }; // 8 m macro -> 2 m runtime texels
    // River cross-section: depth from local width, parabolic bed, then a
    // bank shoulder blending back into the terrain.
    f32 riverDepthCoef { 0.18f }; // depth = coef * (2 * halfWidth)
    f32 riverDepthMin { 0.6f };
    f32 riverDepthMax { 4.0f };
    f32 bankShoulder { 1.4f }; // extra halfWidths of bank blend
    f32 seaLevel { 21.0f };
    // Derived masks.
    f32 beachBand { 90.0f };     // meters of shore flagged as beach
    f32 wetnessReach { 48.0f };  // meters of damp ground around water
    f32 flowLogSpan { 4.0f };    // log10 decades of area mapped to [0,1]
    // Mid-frequency relief (baked): keeps 1st-person scale alive between
    // macro texels; the runtime detail noise adds the last octaves.
    f32 reliefAmplitude { 1.1f };
    f32 reliefWavelength { 42.0f };
};

struct FinalizeResult {
    GridSpec fineSpec;
    vector<f32> height; // fine grid
    // Mask channels at the COARSE spec resolution (u8, [0,255]).
    vector<u8> flow;
    vector<u8> wetness;
    vector<u8> beach;
    vector<u8> detailAmp;
};

// `macro` provides seaDist/biome (S1), `eroded` the S2+S3 heights on the
// `coarse` grid. `hydro` lives on ITS OWN grid `hydroSpec` — since the
// two-stage bake, the hydrology is extracted from the COMPOSED
// neighbourhood window, not from this tile's provisional terrain; grid
// accesses sample it by world coordinates.
FinalizeResult finalizeTerrain(const GridSpec& coarse,
                               const vector<f32>& eroded,
                               const MacroResult& macro,
                               const HydrologyResult& hydro,
                               const GridSpec& hydroSpec,
                               const FinalizeParams& params, u32 seed);

} // namespace render::terraingen
