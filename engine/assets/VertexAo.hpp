#pragma once

#include "engine/assets/MeshData.hpp"

namespace assets {

// Bakes hemispheric SELF-occlusion into MeshVertex.color (multiplied) —
// the BotW recipe (option B, 2026-07-10): ambient grounding lives in the
// assets, not in a screen-space pass (the sampled SSAO's speckle and the
// unsharp-mask's halos both retired from the default look). Static
// meshes only (a deformed skin would carry a stale bake).
//
// Per vertex: `rayCount` fixed golden-spiral rays over the normal
// hemisphere against the mesh's own triangles; a hit within
// `maxDistance` occludes with linear falloff. Brute force O(V*N*T) —
// fine for this game's low-poly assets (a 320-face canopy bakes in
// milliseconds); run it where the mesh is BUILT (veg variant creation,
// the MeshCache decode worker), never per frame.
void bakeVertexAo(render::MeshData& mesh, f32 strength = 0.6f,
                  u32 rayCount = 16, f32 maxDistance = 2.5f);

} // namespace assets
