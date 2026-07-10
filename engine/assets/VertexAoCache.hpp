#pragma once

#include <filesystem>
#include <optional>

#include "engine/assets/MeshData.hpp"

namespace assets {

// Disk cache for the per-vertex AO bake (dev ask 2026-07-10: authored
// kit meshes take SECONDS each to bake — startup was crawling). One
// binary entry per source mesh in a cache directory:
//   <cacheDir>/<fnv1a64-of-source-path>.ao
// An entry stores the source path, its mtime+size and the bake
// parameters; it is valid only when ALL match — touch the glTF or
// change the params and the mesh re-bakes once. Entries hold RAW
// occlusion fractions (strength-free): retuning the apply strength
// never invalidates the cache.

struct VertexAoBakeDesc {
    u32 rayCount { 16 };
    f32 maxDistance { 2.5f };
};

// The one-call path (the MeshCache decode worker): load a valid entry
// or compute + store, then multiply into the vertex colors. Cache IO
// failures degrade to a plain in-memory bake (never fatal).
void applyCachedVertexAo(render::MeshData& mesh,
                         const std::filesystem::path& sourcePath,
                         const std::filesystem::path& cacheDir,
                         f32 strength,
                         const VertexAoBakeDesc& desc = {});

// The halves, exposed for tests and tools.
std::optional<vector<f32>> loadVertexAoCache(
    const std::filesystem::path& cacheDir,
    const std::filesystem::path& sourcePath, const VertexAoBakeDesc& desc,
    u32 vertexCount);
bool saveVertexAoCache(const std::filesystem::path& cacheDir,
                       const std::filesystem::path& sourcePath,
                       const VertexAoBakeDesc& desc,
                       const vector<f32>& occlusion);

// Startup sweep: deletes every entry whose recorded source mesh no
// longer exists. Returns the number of entries removed.
u32 pruneVertexAoCache(const std::filesystem::path& cacheDir);

} // namespace assets
