#pragma once

#include "engine/assets/MeshData.hpp"

namespace assets {

// Decimates in place toward ~targetTriangles (meshoptimizer quadric
// simplifier; the sloppy variant takes over when topology resists), then
// compacts the vertex buffer to the surviving set. Scanned props arrive
// at 40-100k tris; clutter budgets are hundreds (docs/GRASS-REDO.md).
void simplifyMesh(render::MeshData& mesh, u32 targetTriangles);

} // namespace assets
