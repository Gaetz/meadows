#include "engine/assets/CookedMesh.hpp"

#include <cstring>
#include <fstream>

#include "engine/core/Log.hpp"

namespace assets {

namespace {

constexpr char kMagic[4] = { 'C', 'M', 'S', 'H' };
constexpr u32 kMaxCounts = 64u * 1024u * 1024u; // sanity bound, not a design limit

} // namespace

bool saveCookedMesh(const std::filesystem::path& path,
                    const render::MeshData& mesh, u32 contentVersion) {
    if (mesh.vertices.empty() || mesh.indices.empty() ||
        mesh.indices.size() % 3 != 0) {
        LOG_ERROR("saveCookedMesh: malformed mesh for {}", path.string());
        return false;
    }
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        LOG_ERROR("saveCookedMesh: cannot open {}", path.string());
        return false;
    }
    Vec3 boundsMin = mesh.vertices[0].position;
    Vec3 boundsMax = boundsMin;
    for (const render::MeshVertex& v : mesh.vertices) {
        boundsMin = glm::min(boundsMin, v.position);
        boundsMax = glm::max(boundsMax, v.position);
    }
    const auto write = [&](const auto& value) {
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    file.write(kMagic, 4);
    write(kCookedMeshFormatVersion);
    write(contentVersion);
    write(static_cast<u32>(mesh.vertices.size()));
    write(static_cast<u32>(mesh.indices.size()));
    write(boundsMin);
    write(boundsMax);
    file.write(reinterpret_cast<const char*>(mesh.vertices.data()),
               static_cast<std::streamsize>(mesh.vertices.size() *
                                            sizeof(render::MeshVertex)));
    file.write(reinterpret_cast<const char*>(mesh.indices.data()),
               static_cast<std::streamsize>(mesh.indices.size() *
                                            sizeof(u32)));
    return static_cast<bool>(file);
}

namespace {

std::optional<render::MeshData> loadCookedMeshImpl(
    const std::filesystem::path& path, const u32* expectedContentVersion) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        LOG_ERROR("loadCookedMesh: cannot open {}", path.string());
        return std::nullopt;
    }
    char magic[4] = {};
    u32 formatVersion = 0;
    u32 contentVersion = 0;
    u32 vertexCount = 0;
    u32 indexCount = 0;
    Vec3 boundsMin {};
    Vec3 boundsMax {};
    const auto read = [&](auto& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
    };
    file.read(magic, 4);
    read(formatVersion);
    read(contentVersion);
    read(vertexCount);
    read(indexCount);
    read(boundsMin);
    read(boundsMax);
    if (!file || std::memcmp(magic, kMagic, 4) != 0 ||
        formatVersion != kCookedMeshFormatVersion) {
        LOG_ERROR("loadCookedMesh: not a CMSH v{} file: {}",
                  kCookedMeshFormatVersion, path.string());
        return std::nullopt;
    }
    if (expectedContentVersion && contentVersion != *expectedContentVersion) {
        LOG_ERROR("loadCookedMesh: stale content v{} (want v{}): {}",
                  contentVersion, *expectedContentVersion, path.string());
        return std::nullopt;
    }
    if (vertexCount == 0 || indexCount == 0 || indexCount % 3 != 0 ||
        vertexCount > kMaxCounts || indexCount > kMaxCounts) {
        LOG_ERROR("loadCookedMesh: bad counts in {}", path.string());
        return std::nullopt;
    }
    render::MeshData mesh;
    mesh.vertices.resize(vertexCount);
    mesh.indices.resize(indexCount);
    file.read(reinterpret_cast<char*>(mesh.vertices.data()),
              static_cast<std::streamsize>(vertexCount *
                                           sizeof(render::MeshVertex)));
    file.read(reinterpret_cast<char*>(mesh.indices.data()),
              static_cast<std::streamsize>(indexCount * sizeof(u32)));
    if (!file) {
        LOG_ERROR("loadCookedMesh: truncated file: {}", path.string());
        return std::nullopt;
    }
    for (const u32 index : mesh.indices) {
        if (index >= vertexCount) {
            LOG_ERROR("loadCookedMesh: index out of range in {}",
                      path.string());
            return std::nullopt;
        }
    }
    return mesh;
}

} // namespace

std::optional<render::MeshData> loadCookedMesh(
    const std::filesystem::path& path, u32 expectedContentVersion) {
    return loadCookedMeshImpl(path, &expectedContentVersion);
}

std::optional<render::MeshData> loadCookedMesh(
    const std::filesystem::path& path) {
    return loadCookedMeshImpl(path, nullptr);
}

} // namespace assets
