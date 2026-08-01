#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/terrain/WaterBodies.hpp"

// The camera-local water-info map (the Unreal Water lesson): per-texel
// surface height, depth and composited flow for every LOCAL body (lakes
// + rivers — the sea keeps its analytic plane and the pool-depth map).
// Junctions and ribbon overlaps resolve HERE, per texel, by the same
// bank-weighted blend waterFlowAt uses — the render side samples the
// result instead of fighting overlapping geometry. One CPU worker bake
// feeds both GPU textures; this module stays headless and doctested.

namespace render::terrain {

constexpr f32 kWaterInfoDry = -1.0e6f; // surface sentinel: no local water

struct WaterInfoMap {
    Vec2 center { 0.0f, 0.0f }; // texel-snapped world center
    f32 span { 0.0f };          // meters covered per side
    u32 size { 0 };             // texels per side
    vector<f32> surface; // absolute water Y; kWaterInfoDry = dry
    vector<f32> depth;   // surface - ground, meters, >= 0
    vector<Vec2> flow;   // m/s, bank-weighted blend of covering rivers
};

using HeightFn = std::function<f32(f32, f32)>;

// Rasterizes per BODY — cost scales with the wet area, and `height` is
// only called on wet texels. Rivers run through subdivideRiverNodes
// first: the exact curve the ribbon mesh renders. Deterministic (fixed
// body order, fixed texel order).
WaterInfoMap bakeWaterInfo(const WaterBodies& bodies, Vec2 center,
                           f32 span, u32 size, const HeightFn& height);

} // namespace render::terrain
