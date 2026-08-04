#pragma once

#include "engine/assets/MeshData.hpp"

namespace assets {

// Decimates in place toward ~targetTriangles (meshoptimizer quadric
// simplifier; the sloppy variant takes over when topology resists), then
// compacts the vertex buffer to the surviving set. Scanned props arrive
// at 40-100k tris; clutter budgets are hundreds (docs/GRASS-REDO.md).
void simplifyMesh(render::MeshData& mesh, u32 targetTriangles);

// Recenter the mesh on its XZ centroid, floor its base at y = 0 and
// uniform-scale so the LARGEST extent equals `size` meters — scans have
// arbitrary origins and real-world sizes; the scatter's instance scales
// assume normalized footprints (the generateRock convention).
void normalizeMeshFootprint(render::MeshData& mesh, f32 size);

} // namespace assets
