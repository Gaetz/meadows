#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
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
    // A water carve (bed, bank shoulder, lake bed) never lowers a texel
    // by more than its depth plus this bank: ground standing well
    // above the waterline is a hillside or a cliff the ribbon clips,
    // never a trench to dig (a stream along a 270 m zone wall notched
    // it to the waterline over a 2 m band).
    f32 bankMaxCut { 3.0f };
    // The most a bank SHOULDER texel may lose (m). The shoulder blends
    // the bank back into the hillside; unbounded, it brought every
    // wall within reach of a course down to its waterline — 400 m
    // cuts along the fleuves, a quarter of a slice's ground moved
    // (measured). Floodplain bumps flatten, cliffs stand.
    f32 shoulderCutMax { 12.0f };
    // The most a BED texel may lose (m): the course is as wide as its
    // discharge law says, its valley as narrow as the erosion dug it
    // (one texel in the steep country) — the bed band crossed the
    // walls and cut them to the water (400 m, measured). The bed
    // reaches its depth on the floor; against a wall it stops here
    // and the ribbon beyond is under the rock.
    f32 bedCutMax { 20.0f };
    // Publication gate: a course point whose surface hangs more than
    // max(ribbonHangFloor, 1.5 x half-width) above the FINAL fine
    // ground publishes a half-width of 0 — no ribbon, no sim pin, no
    // brook film. The water profile is smoother than the gorge floor
    // the carves dig (a bed 12-20 m under the water along a steep
    // ramp, measured), and a ribbon drawn at that surface is a sheet
    // in the air over a dry gorge; the sim refuses such cells too.
    // Off = legacy, everything publishes.
    bool ribbonHangGate { true };
    f32 ribbonHangFloor { 6.0f };
    f32 seaLevel { kDefaultSeaLevel };
    // Derived masks.
    f32 beachBand { 90.0f };     // meters of shore flagged as beach
    f32 wetnessReach { 48.0f };  // meters of damp ground around water
    f32 flowLogSpan { 4.0f };    // log10 decades of area mapped to [0,1]
    // Mid-frequency relief (baked): keeps 1st-person scale alive between
    // macro texels; the runtime detail noise adds the last octaves.
    f32 reliefAmplitude { 1.1f };
    f32 reliefWavelength { 42.0f };
    // Drainage-preserving upsample: along every coarse cell's D8
    // descent to its receiver, the fine ground is lowered to the
    // linear profile between the two cells inside this half-width
    // (35-degree sides). A texel-wide coarse channel interpolated
    // bicubically became a chain of closed bowls as deep as the
    // channel (hundreds of metres at a pierced gorge), which the map
    // hydrology then drained through kilometres of window-dependent
    // trench — the seam at every map line. 0 = off (legacy).
    f32 drainageCarveHalf { 8.0f };
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
    // Every scalar; `fine` (nested params) is hashed explicitly in
    // hashKey — reflection is flat.
    REFLECT_BEGIN(FinalizeParams, void)
        REFLECT_FIELD(upsampleFactor)
        REFLECT_FIELD(riverDepthCoef)
        REFLECT_FIELD(riverDepthMin)
        REFLECT_FIELD(riverDepthMax)
        REFLECT_FIELD(bankShoulder)
        REFLECT_FIELD(streamDepthMax)
        REFLECT_FIELD(fleuveDepthMax)
        REFLECT_FIELD(fleuveBankShoulder)
        REFLECT_FIELD(fordDepth)
        REFLECT_FIELD(fordRadius)
        REFLECT_FIELD(bankMaxCut)
        REFLECT_FIELD(shoulderCutMax)
        REFLECT_FIELD(bedCutMax)
        REFLECT_FIELD(ribbonHangGate)
        REFLECT_FIELD(ribbonHangFloor)
        REFLECT_FIELD(seaLevel)
        REFLECT_FIELD(beachBand)
        REFLECT_FIELD(wetnessReach)
        REFLECT_FIELD(flowLogSpan)
        REFLECT_FIELD(reliefAmplitude)
        REFLECT_FIELD(reliefWavelength)
        REFLECT_FIELD(drainageCarveHalf)
        REFLECT_FIELD(fineMinX)
        REFLECT_FIELD(fineMinZ)
        REFLECT_FIELD(fineSpan)
        REFLECT_FIELD(lakeDepthCoef)
        REFLECT_FIELD(lakeDepthMax)
        REFLECT_FIELD(strataAmplitude)
        REFLECT_FIELD(strataPeriod)
    REFLECT_END()
};

u64 hashKey(const FinalizeParams& p);

// The drainage-preserving carve of a grid resampled from a coarser one
// (FinalizeParams::drainageCarveHalf): for each coarse cell with a
// strictly lower D8 neighbour, the target texels within `half` of the
// segment joining the two cell centres are lowered to the linear
// profile between the cells plus a 35-degree lateral rise. Lowering
// only, per cell pair, bounded support. Shared by the slice finalize
// (the published ground) and the map hydrology window past the rect
// (the stage-1 resampled at the routing texel): a texel-wide coarse
// channel resampled finer is otherwise a chain of closed bowls.
void carveCoarseDrainage(const GridSpec& coarse, const vector<f32>& eroded,
                         const GridSpec& target, vector<f32>& grid,
                         f32 half);

struct FinalizeResult {
    GridSpec fineSpec;
    vector<f32> height; // fine grid
    // Mask channels at the COARSE spec resolution (u8, [0,255]).
    vector<u8> flow;
    vector<u8> wetness;
    vector<u8> beach;
    vector<u8> detailAmp;
    vector<u8> rockExposure;
    // Bake-internal, never serialized: per-coarse-cell fine-incision
    // norm [0,1] from the ground pass — the water pass's S6 masks read
    // it (wetness up, detail down in fresh ravines).
    vector<f32> incisionNorm;
};

// THE two-phase finalize (restructuration eau 2026-10-12, PAYSAGE
// §7.8): the GROUND pass makes the final terrain with no knowledge of
// water (fine erosion, relief, strata — only the SEA damps relief
// near shores); the map hydrology then runs on this final ground; the
// WATER pass carves the beds/basins/ponds of that hydrology and fills
// the water-aware masks. Water placed on the ground it lives on is
// correct BY CONSTRUCTION — the old order (hydrology on stage-1,
// ground reshaped after) needed a pile of reconcile/clamp correctors
// downstream, each masking the next divergence.
// `drainage`/`drainageSpec`: an accumulation field (drainageArea on
// the stage-1 window) steering the fine ravines toward the macro
// drainage — approximate ground-shaping info, NOT water placement.
FinalizeResult finalizeGround(const GridSpec& coarse,
                              const vector<f32>& eroded,
                              const MacroResult& macro,
                              const vector<f32>* drainage,
                              const GridSpec* drainageSpec,
                              const FinalizeParams& params, u32 seed,
                              const vector<f32>* fineScale = nullptr,
                              const std::atomic<bool>* cancel = nullptr);

// `macro` provides seaDist (beach band); `hydro` lives on ITS OWN grid
// `hydroSpec`, sampled by world coordinates; `deposit` the coarse
// sediment field — scree stays soil-covered in the rockExposure mask.
void finalizeWater(FinalizeResult& out, const GridSpec& coarse,
                   const vector<f32>& eroded, const MacroResult& macro,
                   const HydrologyResult& hydro,
                   const GridSpec& hydroSpec,
                   const FinalizeParams& params, u32 seed,
                   const vector<f32>* deposit = nullptr);

// Legacy single-call wrapper (benches/tests): ground pass then water
// pass. NOTE: the ground pass no longer protects lake/river corridors
// from fine erosion or relief (only the sea does) — the carves own
// the channels now.
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
