#pragma once

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

ThermalResult erodeThermal(const GridSpec& spec, const vector<f32>& height,
                           const ThermalParams& params,
                           const vector<f32>* talusScale = nullptr);

} // namespace render::terraingen
