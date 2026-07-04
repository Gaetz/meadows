#pragma once

#include <filesystem>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/render/MeshData.hpp"

namespace assets {

// glTF 2.0 → render::MeshData (brick 23). Static meshes only: every node's
// triangle primitives are flattened into one buffer with world transforms
// applied, so the result plugs into the landscape instancing path like any
// generated mesh. Reads POSITION/NORMAL/TEXCOORD_0/COLOR_0; the material's
// baseColorFactor multiplies into the vertex color (the landscape shaders
// are untextured — color rides on the vertex). No skinning, no animation.
// Returns nullopt with a logged error on malformed input.
std::optional<render::MeshData> loadGltfMesh(
    const std::filesystem::path& path);

// Same, from an in-memory .glb/.gltf blob (embedded buffers only — no
// external .bin fetches). Powers the headless doctest.
std::optional<render::MeshData> loadGltfMeshFromMemory(const void* bytes,
                                                       u64 byteCount);

// Fits an authored mesh to the landscape's prop conventions: recenters the
// footprint on XZ, drops the lowest point to y = 0 (scatter places the base
// on the terrain), and uniformly scales so the largest dimension equals
// `targetSize` meters (scale 1 instances then read as authored).
void normalizeMesh(render::MeshData& mesh, f32 targetSize);

} // namespace assets
