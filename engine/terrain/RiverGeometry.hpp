#pragma once

#include "engine/core/Defines.hpp"
#include "engine/terrain/WaterBodies.hpp" // render::RiverNode

// Presentation-side conditioning of river polylines. Headless on
// purpose: the ribbon mesh (WaterSystem) and the water-info raster must
// sample the SAME curve, or the rendered surface and the composited
// data would disagree at bends. Gameplay queries keep the raw polyline
// — the deviation is bounded by the angle cap (well under a meter).

namespace render::terrain {

// Adaptive subdivision by curvature: inserts Catmull-Rom (xz)
// interpolated nodes until the tangent turn per emitted segment stays
// under `maxAngle` radians. surface/halfWidth are lerped (the monotone
// downhill surface is preserved); straight reaches come back unchanged.
// A trailing pass clamps halfWidth by the local turn radius so the
// inner bank of a tight bend can never fold over itself.
vector<RiverNode> subdivideRiverNodes(const vector<RiverNode>& nodes,
                                      f32 maxAngle = 0.14f,
                                      f32 minStep = 2.0f,
                                      u32 maxInserts = 8);

} // namespace render::terrain
