#pragma once

#include <functional>

#include <glm/glm.hpp>

#include "engine/assets/MeshData.hpp"
#include "engine/core/Defines.hpp"

// Stage D4 of the dungeon pipeline (docs/DUNGEON-GEN.md) — isosurface
// extraction of the carve field, one chunk at a time. SURFACE NETS rather
// than classic marching cubes: no case tables, one vertex per crossed cell
// (centroid of edge crossings — smoother on organic rock), quads per crossed
// lattice edge. The seam contract is the point:
//   - the voxel lattice is GLOBAL (indices from world origin, never from the
//     chunk corner), so two chunks sample identical corner values;
//   - a quad belongs to the chunk that owns its lattice edge's midpoint;
//     boundary vertices are recomputed identically on both sides.
// Together: adjacent chunk meshes are watertight by construction.

namespace dungeon {

using DensityFn = std::function<f32(const Vec3&)>;

// Triangles of the surface crossing [chunkMin, chunkMax). Vertices in WORLD
// coordinates (callers re-base per cell); normals face the carved air (the
// negative side of the density); colors default to white (stage D6 tints).
render::MeshData extractChunkMesh(const DensityFn& density,
                                  const Vec3& chunkMin, const Vec3& chunkMax,
                                  f32 voxelSize);

} // namespace dungeon
