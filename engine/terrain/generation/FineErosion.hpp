#pragma once

#include <atomic>

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Fine-scale erosion amplification (finalize stage): a stream-power
// carve with STRICTLY BOUNDED support that adds real ravines and rills
// between the macro texels, where pure detail noise used to stand in.
// Two deliberate departures from the S2 solver keep the pass local —
// the property that lets neighbouring tile bakes agree in their shared
// band:
//   - receivers are raw steepest descent (no priority flood): cells
//     with no lower neighbour do not carve — pits are deposition zones
//     and the coarse pass already gave them flat floors;
//   - drainage accumulation runs a fixed number of gather sweeps, so a
//     texel's carve depends on at most reachSteps * texelSize meters of
//     upstream terrain. Large-scale variation comes from the COARSE
//     discharge field (shared across tiles by construction), not from
//     the local accumulation.
// Formulated as ordered gathers/Jacobi on purpose: the cheap shape to
// port to compute if the budget ever demands it.

namespace render::terraingen {

struct FineErosionParams {
    i32 iterations { 10 };
    i32 routingInterval { 3 }; // receiver/accumulation refresh period
    i32 reachSteps { 24 };     // accumulation reach = steps * texelSize
    // Incision dh = dt * k * min(a, areaCap)^areaExponent * slope
    //             * (1 + dischargeGain * discharge) * allow, capped by
    // maxDepth cumulatively and by a fraction of the local drop per
    // step (explicit scheme: never invert a slope).
    f32 dt { 1.0f };
    f32 k { 0.012f };
    f32 areaExponent { 0.45f };
    f32 areaCap { 9.0e3f }; // m² where local accumulation saturates
    f32 dischargeGain { 0.6f };
    f32 maxDepth { 3.5f }; // total carve budget per texel (m)
    // Micro-thermal pass over the final fine grid (runs in finalize via
    // erodeThermal): breaks ravine walls into credible talus facets.
    i32 thermalIterations { 5 };
    f32 thermalTalusTan { 0.8f };
};

struct FineErosionResult {
    vector<f32> height;
    vector<f32> incision; // cumulative carve per texel (m) — mask input
};

// `allow` in [0,1] gates the carve per texel (0 = protected: river beds,
// lakes, beaches, sea). `discharge` is the log-normalized COARSE flow
// field resampled to `spec`. `scale`, if given, multiplies k per texel
// (biome fineScale). Deterministic: fixed row-major sweeps only.
FineErosionResult amplifyFine(const GridSpec& spec,
                              const vector<f32>& height,
                              const vector<f32>& allow,
                              const vector<f32>& discharge,
                              const FineErosionParams& params,
                              const vector<f32>* scale = nullptr,
                              const std::atomic<bool>* cancel = nullptr);

} // namespace render::terraingen
