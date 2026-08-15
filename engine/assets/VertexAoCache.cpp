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

// Source fingerprint; nullopt when the file is unreadable. Content-keyed
// entries (no source file) use a zero fingerprint.
struct Fingerprint {
    u64 mtime { 0 };
    u64 size { 0 };
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
                                   const str& keyUtf8) {
    // fnv1a over the key string (a generic path, or "key:<hash>" for
    // content-keyed entries); the header stores the full key, so a
    // collision only costs a re-bake.
    char name[32];
    std::snprintf(name, sizeof(name), "%08x.ao", core::fnv1a(keyUtf8));
    return cacheDir / name;
}

// An entry is valid only when params, fingerprint, vertex count AND the
// stored key all match — anything else reads as "stale, re-bake".
std::optional<vector<f32>> readEntry(const std::filesystem::path& cacheDir,
                                     const str& keyUtf8,
                                     const VertexAoBakeDesc& desc,
                                     u32 vertexCount,
                                     const Fingerprint& fingerprint) {
    std::ifstream in(entryPathFor(cacheDir, keyUtf8), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    Header header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || header.magic != kMagic ||
        header.rayCount != desc.rayCount ||
        header.maxDistance != desc.maxDistance ||
        header.sourceMtime != fingerprint.mtime ||
        header.sourceSize != fingerprint.size ||
        header.vertexCount != vertexCount || header.pathLength == 0 ||
        header.pathLength > 4096) {
        return std::nullopt;
    }
    str storedKey(header.pathLength, '\0');
    in.read(storedKey.data(), header.pathLength);
    if (!in || storedKey != keyUtf8) {
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

bool writeEntry(const std::filesystem::path& cacheDir, const str& keyUtf8,
                const VertexAoBakeDesc& desc, const Fingerprint& fingerprint,
                const vector<f32>& occlusion) {
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    std::ofstream out(entryPathFor(cacheDir, keyUtf8),
                      std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    Header header;
    header.rayCount = desc.rayCount;
    header.maxDistance = desc.maxDistance;
    header.sourceMtime = fingerprint.mtime;
    header.sourceSize = fingerprint.size;
    header.vertexCount = static_cast<u32>(occlusion.size());
    header.pathLength = static_cast<u32>(keyUtf8.size());
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(keyUtf8.data(),
              static_cast<std::streamsize>(keyUtf8.size()));
    out.write(reinterpret_cast<const char*>(occlusion.data()),
              static_cast<std::streamsize>(occlusion.size() * sizeof(f32)));
    return static_cast<bool>(out);
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
    return readEntry(cacheDir, sourcePath.generic_string(), desc,
                     vertexCount, *fingerprint);
}

bool saveVertexAoCache(const std::filesystem::path& cacheDir,
                       const std::filesystem::path& sourcePath,
                       const VertexAoBakeDesc& desc,
                       const vector<f32>& occlusion) {
    const auto fingerprint = fingerprintOf(sourcePath);
    if (!fingerprint) {
        return false;
    }
    return writeEntry(cacheDir, sourcePath.generic_string(), desc,
                      *fingerprint, occlusion);
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

void applyContentKeyedVertexAo(render::MeshData& mesh,
                               const std::filesystem::path& cacheDir,
                               f32 strength,
                               const VertexAoBakeDesc& desc) {
    if (strength <= 0.0f || mesh.vertices.empty()) {
        return;
    }
    // Content key: fnv1a over the vertex positions (stride-safe: hash
    // the position field bytes per vertex, not the padded struct).
    u32 hash = 2166136261u;
    for (const render::MeshVertex& vertex : mesh.vertices) {
        const char* bytes = reinterpret_cast<const char*>(&vertex.position);
        for (size_t i = 0; i < sizeof(Vec3); ++i) {
            hash = (hash ^ static_cast<unsigned char>(bytes[i])) *
                   16777619u;
        }
    }
    char key[64];
    std::snprintf(key, sizeof(key), "key:%08x:%zu", hash,
                  mesh.vertices.size());
    const str keyUtf8 { key };
    const u32 vertexCount = static_cast<u32>(mesh.vertices.size());
    // Keyed entries validate by content hash alone: zero fingerprint.
    if (const auto cached =
            readEntry(cacheDir, keyUtf8, desc, vertexCount, {})) {
        applyVertexOcclusion(mesh, *cached, strength);
        return;
    }
    const vector<f32> occlusion =
        computeVertexOcclusion(mesh, desc.rayCount, desc.maxDistance);
    writeEntry(cacheDir, keyUtf8, desc, {}, occlusion);
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
                // Content-keyed entries (procedural meshes) have no
                // source file — never pruned.
                drop = storedPath.rfind("key:", 0) != 0 &&
                       !std::filesystem::exists(
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
