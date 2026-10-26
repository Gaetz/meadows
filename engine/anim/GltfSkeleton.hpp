#pragma once

#include <filesystem>
#include <optional>

#include "engine/anim/Anim.hpp"
#include "engine/core/Defines.hpp"

// glTF 2.0 -> the anim runtime's plain structs. Skin 0 of the file becomes
// the skeleton (joints reordered parents-first, the runtime's requirement);
// animations resample into engine clips with tracks indexed by joint.
// Skinned MESH import (vertex weights) is assets::loadGltfSkinnedMesh,
// which applies the SAME joint reorder (assets/GltfParse.hpp).

namespace anim {

// Named clip: `name` comes from the glTF animation (may be empty).
struct GltfClip {
    str name;
    AnimClip clip;
};

std::optional<Skeleton> loadGltfSkeleton(const std::filesystem::path& path);
vector<GltfClip> loadGltfAnimations(const std::filesystem::path& path,
                                    const Skeleton& skeleton);

} // namespace anim
