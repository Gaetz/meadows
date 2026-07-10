#include "engine/assets/VertexAoCache.hpp"

#include <cstring>
#include <fstream>

#include "engine/assets/VertexAo.hpp"
#include "engine/core/Hash.hpp"
#include "engine/core/Log.hpp"

namespace assets {

namespace {

constexpr u32 kMagic = 0x314F414Du; // 'MAO1'

struct Header {
    u32 magic { kMagic };
    u32 rayCount { 0 };
    f32 maxDistance { 0.0f };
    u64 sourceMtime { 0 };
    u64 sourceSize { 0 };
    u32 vertexCount { 0 };
    u32 pathLength { 0 }; // UTF-8 source path follows, then f32[vertexCount]
};

// Source fingerprint; nullopt when the file is unreadable.
struct Fingerprint {
    u64 mtime;
    u64 size;
};
std::optional<Fingerprint> fingerprintOf(const std::filesystem::path& path) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return Fingerprint {
        static_cast<u64>(time.time_since_epoch().count()),
        static_cast<u64>(size)
    };
}

std::filesystem::path entryPathFor(const std::filesystem::path& cacheDir,
                                   const str& sourceUtf8) {
    // fnv1a over the generic (forward-slash) path string; the header
    // stores the full path, so a collision only costs a re-bake.
    char name[32];
    std::snprintf(name, sizeof(name), "%08x.ao", core::fnv1a(sourceUtf8));
    return cacheDir / name;
}

} // namespace

std::optional<vector<f32>> loadVertexAoCache(
    const std::filesystem::path& cacheDir,
    const std::filesystem::path& sourcePath, const VertexAoBakeDesc& desc,
    u32 vertexCount) {
    const auto fingerprint = fingerprintOf(sourcePath);
    if (!fingerprint) {
        return std::nullopt;
    }
    const str sourceUtf8 = sourcePath.generic_string();
    std::ifstream in(entryPathFor(cacheDir, sourceUtf8),
                     std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Header header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || header.magic != kMagic ||
        header.rayCount != desc.rayCount ||
        header.maxDistance != desc.maxDistance ||
        header.sourceMtime != fingerprint->mtime ||
        header.sourceSize != fingerprint->size ||
        header.vertexCount != vertexCount || header.pathLength == 0 ||
        header.pathLength > 4096) {
        return std::nullopt; // stale/foreign entry: caller re-bakes
    }
    str storedPath(header.pathLength, '\0');
    in.read(storedPath.data(), header.pathLength);
    if (!in || storedPath != sourceUtf8) {
        return std::nullopt; // hash collision — re-bake
    }
    vector<f32> occlusion(vertexCount);
    in.read(reinterpret_cast<char*>(occlusion.data()),
            static_cast<std::streamsize>(vertexCount * sizeof(f32)));
    if (!in) {
        return std::nullopt;
    }
    return occlusion;
}

bool saveVertexAoCache(const std::filesystem::path& cacheDir,
                       const std::filesystem::path& sourcePath,
                       const VertexAoBakeDesc& desc,
                       const vector<f32>& occlusion) {
    const auto fingerprint = fingerprintOf(sourcePath);
    if (!fingerprint) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    const str sourceUtf8 = sourcePath.generic_string();
    std::ofstream out(entryPathFor(cacheDir, sourceUtf8),
                      std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    Header header;
    header.rayCount = desc.rayCount;
    header.maxDistance = desc.maxDistance;
    header.sourceMtime = fingerprint->mtime;
    header.sourceSize = fingerprint->size;
    header.vertexCount = static_cast<u32>(occlusion.size());
    header.pathLength = static_cast<u32>(sourceUtf8.size());
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(sourceUtf8.data(),
              static_cast<std::streamsize>(sourceUtf8.size()));
    out.write(reinterpret_cast<const char*>(occlusion.data()),
              static_cast<std::streamsize>(occlusion.size() * sizeof(f32)));
    return static_cast<bool>(out);
}

void applyCachedVertexAo(render::MeshData& mesh,
                         const std::filesystem::path& sourcePath,
                         const std::filesystem::path& cacheDir,
                         f32 strength, const VertexAoBakeDesc& desc) {
    if (strength <= 0.0f || mesh.vertices.empty()) {
        return;
    }
    const u32 vertexCount = static_cast<u32>(mesh.vertices.size());
    if (const auto cached =
            loadVertexAoCache(cacheDir, sourcePath, desc, vertexCount)) {
        applyVertexOcclusion(mesh, *cached, strength);
        return;
    }
    const vector<f32> occlusion =
        computeVertexOcclusion(mesh, desc.rayCount, desc.maxDistance);
    if (!saveVertexAoCache(cacheDir, sourcePath, desc, occlusion)) {
        LOG_WARN("vertex AO cache: could not store '{}' (baking every "
                 "launch)",
                 sourcePath.generic_string());
    }
    applyVertexOcclusion(mesh, occlusion, strength);
}

u32 pruneVertexAoCache(const std::filesystem::path& cacheDir) {
    std::error_code ec;
    u32 removed = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(cacheDir, ec)) {
        if (entry.path().extension() != ".ao") {
            continue;
        }
        bool drop = true; // unreadable/corrupt entries go too
        std::ifstream in(entry.path(), std::ios::binary);
        Header header;
        if (in.read(reinterpret_cast<char*>(&header), sizeof(header)) &&
            header.magic == kMagic && header.pathLength > 0 &&
            header.pathLength <= 4096) {
            str storedPath(header.pathLength, '\0');
            if (in.read(storedPath.data(), header.pathLength)) {
                drop = !std::filesystem::exists(
                    std::filesystem::path(storedPath), ec);
            }
        }
        if (drop) {
            in.close();
            if (std::filesystem::remove(entry.path(), ec)) {
                ++removed;
            }
        }
    }
    return removed;
}

} // namespace assets
