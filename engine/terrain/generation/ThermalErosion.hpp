#pragma once

#include <atomic>

#include "engine/core/Defines.hpp"
#include "engine/terrain/generation/TerrainGen.hpp"

// Stage S3 — thermal erosion (Musgrave/Olsen talus relaxation): material
// above the angle of repose slides to the lowest neighbour. Straightens
// scree aprons under the stream-power cliffs and removes single-texel
// spikes. The cumulative deposit doubles as the scree/sediment mask.

namespace render::terraingen {

struct ThermalParams {
    i32 iterations { 60 };
    f32 rate { 0.5f }; // fraction of the excess moved per pass
    // tan(angle of repose): ~0.65 = 33° scree. Scaled per texel by the
    // optional `talusScale` grid (biome character: hard rock holds
    // steeper faces).
    f32 talusTan { 0.65f };
    // Cells at/below the sea never erode (base level) but can receive
    // slide material — cliffs shed scree into the water.
    f32 seaLevel { kDefaultSeaLevel };
};

struct ThermalResult {
    vector<f32> height;
    vector<f32> deposit; // meters of received material (scree mask)
};

// `cancel` (raised = abort between iterations, partial result) is for
// shutdown only — callers must discard, never cache or publish.
ThermalResult erodeThermal(const GridSpec& spec, const vector<f32>& height,
                           const ThermalParams& params,
                           const vector<f32>* talusScale = nullptr,
                           const std::atomic<bool>* cancel = nullptr);

// Crest rounding: peaks and aretes (cells standing ABOVE their
// neighbourhood mean) relax toward that mean; concave ground — valleys,
// carved beds — is never touched. Material is NOT conserved (shaved
// crests vanish): a stylization pass, not physics.
struct RidgeRoundParams {
    f32 radius { 64.0f };   // meters of neighbourhood-mean support
    f32 strength { 0.6f };  // fraction of the prominence shaved at full weight
    // Prominence band: weight ramps over [0.5, 1.5] x threshold, so
    // gentle convexity (hill tops, terrace rims) passes untouched.
    f32 threshold { 4.0f }; // meters
    f32 seaLevel { kDefaultSeaLevel };
};

// `weight`, if given, scales the effect per cell — the caller gates the
// rounding to where the orogeny built knife edges (uplift).
vector<f32> roundRidges(const GridSpec& spec, const vector<f32>& height,
                        const RidgeRoundParams& params,
                        const vector<f32>* weight = nullptr);

} // namespace render::terraingen
