#pragma once

#include "engine/assets/MeshData.hpp"

namespace assets {

// Bakes hemispheric SELF-occlusion into MeshVertex.color (multiplied) —
// the BotW recipe: ambient grounding lives in the
// assets, not in a screen-space pass (the sampled SSAO's speckle and the
// unsharp-mask's halos both retired from the default look). Static
// meshes only (a deformed skin would carry a stale bake).
//
// Per vertex: `rayCount` fixed golden-spiral rays over the normal
// hemisphere against the mesh's own triangles; a hit within
// `maxDistance` occludes with linear falloff. Brute force O(V*N*T) —
// fine for procedural low-poly assets (a 320-face canopy bakes in
// milliseconds) but SECONDS for authored kit meshes: those go through
// the disk cache (VertexAoCache.hpp) instead of re-baking per launch.
void bakeVertexAo(render::MeshData& mesh, f32 strength = 0.6f,
                  u32 rayCount = 16, f32 maxDistance = 2.5f);

// The two halves, split for the cache: raw occlusion fractions (0 = open,
// 1 = fully occluded — STRENGTH-FREE, so a strength retune never
// invalidates a cache entry) and their application to the vertex colors.
vector<f32> computeVertexOcclusion(const render::MeshData& mesh,
                                   u32 rayCount = 16,
                                   f32 maxDistance = 2.5f);
void applyVertexOcclusion(render::MeshData& mesh,
                          const vector<f32>& occlusion, f32 strength);

} // namespace assets
