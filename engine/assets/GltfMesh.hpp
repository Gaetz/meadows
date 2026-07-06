#pragma once

#include <filesystem>
#include <optional>

#include "engine/anim/Anim.hpp"
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

// The runtime prop-pivot convention (MeshCache applies it to every loaded
// StaticForm model): footprint centered on XZ, base at y = 0 — references
// place the base on the ground — WITHOUT rescaling. Authored size is truth;
// per-instance sizing lives in ReferenceForm.scale.
void groundMesh(render::MeshData& mesh);

// --- Skeletal data (horizontal pass H5) ----------------------------------------
// Skin 0 of the file becomes the skeleton (joints reordered parents-first,
// the anim runtime's requirement); animations resample into engine clips
// with tracks indexed by joint. Skinned MESH import (vertex weights) lands
// with the GPU-skinning vertical.

// Named clip: `name` comes from the glTF animation (may be empty).
struct GltfClip {
    str name;
    anim::AnimClip clip;
};

std::optional<anim::Skeleton> loadGltfSkeleton(
    const std::filesystem::path& path);
vector<GltfClip> loadGltfAnimations(const std::filesystem::path& path,
                                    const anim::Skeleton& skeleton);

// Skinned mesh import (chantier 1, B2): the triangle primitives of every
// node bound to skin 0, vertices left in bind-pose mesh space (a skinned
// node's transform does not apply — the bone palette places vertices), and
// JOINTS_0 remapped through the SAME parents-first reorder as
// loadGltfSkeleton, so palette indices and Skeleton::joints stay aligned.
std::optional<render::SkinnedMeshData> loadGltfSkinnedMesh(
    const std::filesystem::path& path);
std::optional<render::SkinnedMeshData> loadGltfSkinnedMeshFromMemory(
    const void* bytes, u64 byteCount);

} // namespace assets
