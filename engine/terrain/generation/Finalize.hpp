#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/FineErosion.hpp"
#include "engine/terrain/generation/Hydrology.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Stages S5 + S6 — finalization: bicubic upsample of the eroded macro
// grid to runtime texels, river-bed carving along the extracted
// polylines, a mask-modulated mid-frequency relief pass (so the upsample
// does not read as blurred macro), and the derived mask channels the
// splat/scatter/water systems consume.

namespace render::terraingen {

struct FinalizeParams {
    u32 upsampleFactor { 8 }; // 16 m macro -> 2 m runtime texels
    // River cross-section: depth from local width, parabolic bed, then a
    // bank shoulder blending back into the terrain.
    f32 riverDepthCoef { 0.18f }; // depth = coef * (2 * halfWidth)
    f32 riverDepthMin { 0.6f };
    f32 riverDepthMax { 4.0f };
    f32 bankShoulder { 1.4f }; // extra halfWidths of bank blend
    // Per-tier caps (River::tier): the ruisseau stays wadeable
    // everywhere, the rivière entrenches (riverDepth* above), the
    // fleuve is the swim obstacle with a wide shoulder. Ford spots
    // (River::fords) cap the rivière's bed locally — a ford is
    // TERRAIN, crossable on foot, not a gameplay marker.
    f32 streamDepthMax { 0.5f };
    f32 fleuveDepthMax { 7.0f };
    // Wide shoulder = the alluvial floor: the shoulder cap pulls the
    // banks toward the waterline over this many halfWidths, so a grand
    // fleuve flattens its valley bottom instead of sitting in a slot.
    f32 fleuveBankShoulder { 3.2f };
    f32 fordDepth { 0.4f };
    f32 fordRadius { 9.0f }; // full-cap core; the cap fades out by 2x
    f32 seaLevel { kDefaultSeaLevel };
    // Derived masks.
    f32 beachBand { 90.0f };     // meters of shore flagged as beach
    f32 wetnessReach { 48.0f };  // meters of damp ground around water
    f32 flowLogSpan { 4.0f };    // log10 decades of area mapped to [0,1]
    // Mid-frequency relief (baked): keeps 1st-person scale alive between
    // macro texels; the runtime detail noise adds the last octaves.
    f32 reliefAmplitude { 1.1f };
    f32 reliefWavelength { 42.0f };
    // Fine window: the sub-rect of the coarse grid that is upsampled and
    // refined (world meters, clamped to the coarse rect). Zero span =
    // the whole grid (tests, tools). The bake restricts it to
    // tile + margin + fine-erosion halo — upsampling the full apron was
    // wasted work that pays for the fine-erosion pass.
    f32 fineMinX { 0.0f };
    f32 fineMinZ { 0.0f };
    f32 fineSpan { 0.0f };
    // Fine-scale erosion between the macro texels (FineErosion.hpp);
    // iterations = 0 disables the pass AND its micro-thermal follower,
    // restoring the plain bicubic upsample.
    FineErosionParams fine;
    // Lake-bed profile: natural (masked) lakes get a basin carved under
    // their surface — depth grows with the distance to the shore, so
    // banks stay wadeable and the middle is divable. Carve-only (never
    // raises ground); 0 coefficient disables.
    f32 lakeDepthCoef { 0.10f }; // m of depth per m from the shore
    f32 lakeDepthMax { 12.0f };
    // Geological strata. The rockExposure MASK always carries the
    // banding (material-side, zero geometric risk). strataAmplitude
    // additionally displaces the fine grid on very steep faces —
    // shipped OFF: it can beat against the S1 terracing, enable only
    // after a visual review.
    f32 strataAmplitude { 0.0f }; // meters of ledge displacement
    f32 strataPeriod { 14.0f };   // meters of altitude per band
};

struct FinalizeResult {
    GridSpec fineSpec;
    vector<f32> height; // fine grid
    // Mask channels at the COARSE spec resolution (u8, [0,255]).
    vector<u8> flow;
    vector<u8> wetness;
    vector<u8> beach;
    vector<u8> detailAmp;
    vector<u8> rockExposure;
};

// `macro` provides seaDist/biome (S1), `eroded` the S2+S3 heights on the
// `coarse` grid. `hydro` lives on ITS OWN grid `hydroSpec` — since the
// two-stage bake, the hydrology is extracted from the COMPOSED
// neighbourhood window, not from this tile's provisional terrain; grid
// accesses sample it by world coordinates. `fineScale`, if given, is a
// coarse-grid per-texel multiplier on the fine-erosion k (biome
// character); `deposit` the coarse sediment field (fluvial + thermal) —
// scree stays soil-covered in the rockExposure mask.
FinalizeResult finalizeTerrain(const GridSpec& coarse,
                               const vector<f32>& eroded,
                               const MacroResult& macro,
                               const HydrologyResult& hydro,
                               const GridSpec& hydroSpec,
                               const FinalizeParams& params, u32 seed,
                               const vector<f32>* fineScale = nullptr,
                               const vector<f32>* deposit = nullptr,
                               const std::atomic<bool>* cancel = nullptr);

} // namespace render::terraingen
