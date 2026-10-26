#pragma once

#include <filesystem>

#include "engine/core/Defines.hpp"

// The cgltf plumbing shared by the glTF importers (the mesh importers here
// in assets, the skeleton/animation importers in engine/anim): one parsed
// document, one joint reorder. Internal to the engine base library — the
// cgltf types stay forward-declared, so no includer needs <cgltf.h>.
struct cgltf_data;
struct cgltf_skin;

namespace assets {

// RAII cgltf document + the parse/load-buffers boilerplate. File loads
// resolve external buffers next to the file; in-memory loads only resolve
// GLB bin chunks and data: URIs.
struct ParsedGltf {
    cgltf_data* data { nullptr };

    ParsedGltf() = default;
    ParsedGltf(const ParsedGltf&) = delete;
    ParsedGltf& operator=(const ParsedGltf&) = delete;
    ~ParsedGltf();
};

bool parseWithBuffers(const std::filesystem::path& path, ParsedGltf& out);
bool parseFromMemory(const void* bytes, u64 byteCount, ParsedGltf& out);

// Skin joints in glTF come in arbitrary order; the anim runtime wants
// parents before children. Returns old-index -> new-index. The skeleton
// importer and the skinned-mesh importer MUST both use this one function:
// palette indices in the vertices and Skeleton::joints stay aligned only
// because they share the reorder.
vector<u32> topologicalJointOrder(const cgltf_skin& skin);

} // namespace assets
