#include "world/terrain/TerrainPatches.hpp"

#include <cstring>
#include <fstream>

#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

namespace {
constexpr char kMagic[4] = { 'T', 'E', 'R', '1' };
}

bool writeTerFile(const std::filesystem::path& path,
                  const render::HeightPatch& patch) {
    if (patch.samples < 2 ||
        patch.deltas.size() !=
            static_cast<size_t>(patch.samples) * patch.samples) {
        LOG_ERROR("writeTerFile: malformed patch for {}", path.string());
        return false;
    }
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        LOG_ERROR("writeTerFile: cannot open {}", path.string());
        return false;
    }
    file.write(kMagic, 4);
    file.write(reinterpret_cast<const char*>(&patch.samples),
               sizeof(patch.samples));
    file.write(reinterpret_cast<const char*>(patch.deltas.data()),
               static_cast<std::streamsize>(patch.deltas.size() *
                                            sizeof(f32)));
    return static_cast<bool>(file);
}

std::optional<render::HeightPatch> readTerFile(
    const std::filesystem::path& path) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        LOG_ERROR("readTerFile: cannot open {}", path.string());
        return std::nullopt;
    }
    char magic[4] = {};
    u32 samples = 0;
    file.read(magic, 4);
    file.read(reinterpret_cast<char*>(&samples), sizeof(samples));
    if (!file || std::memcmp(magic, kMagic, 4) != 0 || samples < 2 ||
        samples > 4096) {
        LOG_ERROR("readTerFile: not a TER1 grid: {}", path.string());
        return std::nullopt;
    }
    render::HeightPatch patch;
    patch.samples = samples;
    patch.deltas.resize(static_cast<size_t>(samples) * samples);
    file.read(reinterpret_cast<char*>(patch.deltas.data()),
              static_cast<std::streamsize>(patch.deltas.size() *
                                           sizeof(f32)));
    if (!file) {
        LOG_ERROR("readTerFile: truncated grid: {}", path.string());
        return std::nullopt;
    }
    return patch;
}

sptr<const render::HeightPatches> buildHeightPatches(
    const data::FormDatabase& forms, const assets::AssetDatabase& assets,
    f32 chunkSize) {
    auto patches = std::make_shared<render::HeightPatches>();
    patches->chunkSize = chunkSize;
    data::forEach<TerrainPatchForm>(
        forms, [&](const TerrainPatchForm& form) {
            const auto path = assets.resolve(form.asset);
            if (!path) {
                LOG_WARN("TerrainPatch ({}, {}): asset {} not registered",
                         form.chunkX, form.chunkZ, form.asset.toString());
                return;
            }
            auto grid = readTerFile(*path);
            if (!grid) {
                return;
            }
            patches->chunks.emplace(
                render::HeightPatches::keyOf(form.chunkX, form.chunkZ),
                std::move(*grid));
        });
    return patches;
}

} // namespace world
