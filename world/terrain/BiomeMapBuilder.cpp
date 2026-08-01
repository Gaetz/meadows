#include "world/terrain/BiomeMapBuilder.hpp"

#include <cstring>
#include <fstream>

#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

namespace {
constexpr char kMagic[4] = { 'T', 'B', 'M', '1' };
constexpr u32 kMaxSamples = 8192;
} // namespace

bool writeTbmFile(const std::filesystem::path& path,
                  const BiomeIndexMap& map) {
    if (map.width < 2 || map.height < 2 || map.width > kMaxSamples ||
        map.height > kMaxSamples ||
        map.indices.size() !=
            static_cast<size_t>(map.width) * map.height) {
        LOG_ERROR("writeTbmFile: malformed map for {}", path.string());
        return false;
    }
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        LOG_ERROR("writeTbmFile: cannot open {}", path.string());
        return false;
    }
    const auto write = [&](const auto& value) {
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    file.write(kMagic, 4);
    write(map.width);
    write(map.height);
    write(map.originX);
    write(map.originZ);
    write(map.texelSize);
    file.write(reinterpret_cast<const char*>(map.indices.data()),
               static_cast<std::streamsize>(map.indices.size()));
    return static_cast<bool>(file);
}

std::optional<BiomeIndexMap> readTbmFile(
    const std::filesystem::path& path) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        LOG_ERROR("readTbmFile: cannot open {}", path.string());
        return std::nullopt;
    }
    char magic[4] = {};
    BiomeIndexMap map;
    const auto read = [&](auto& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
    };
    file.read(magic, 4);
    read(map.width);
    read(map.height);
    read(map.originX);
    read(map.originZ);
    read(map.texelSize);
    if (!file || std::memcmp(magic, kMagic, 4) != 0 || map.width < 2 ||
        map.height < 2 || map.width > kMaxSamples ||
        map.height > kMaxSamples || map.texelSize <= 0.0f) {
        LOG_ERROR("readTbmFile: not a TBM1 map: {}", path.string());
        return std::nullopt;
    }
    map.indices.resize(static_cast<size_t>(map.width) * map.height);
    file.read(reinterpret_cast<char*>(map.indices.data()),
              static_cast<std::streamsize>(map.indices.size()));
    if (!file) {
        LOG_ERROR("readTbmFile: truncated map: {}", path.string());
        return std::nullopt;
    }
    return map;
}

sptr<const render::BiomeSet> buildBiomeSet(
    const data::FormDatabase& forms, const assets::AssetDatabase& assets) {
    auto set = std::make_shared<render::BiomeSet>();
    set->table.push_back({}); // [0] neutral, always present
    data::forEach<BiomeForm>(forms, [&](const BiomeForm& form) {
        const i32 index = form.paletteIndex;
        if (index < 0 || index > 255) {
            LOG_WARN("Biome '{}': paletteIndex {} out of range",
                     form.displayName, index);
            return;
        }
        if (set->table.size() <= static_cast<size_t>(index)) {
            set->table.resize(static_cast<size_t>(index) + 1);
        }
        render::BiomeParams& params = set->table[static_cast<size_t>(
            index)];
        params.snowLineOffset = form.snowLineOffset;
        params.rockiness = form.rockiness;
        params.sandiness = form.sandiness;
        params.grassPresence = form.grassPresence;
        params.detailAmplitudeScale = form.detailAmplitudeScale;
        params.temperature = form.temperature;
        params.wetness = form.wetness;
        params.vegetationSet = static_cast<u32>(
            glm::max(form.vegetationSet, 0));
    });
    data::forEach<BiomeMapForm>(forms, [&](const BiomeMapForm& form) {
        const auto path = assets.resolve(form.asset);
        if (!path) {
            LOG_WARN("BiomeMap: asset {} not registered",
                     form.asset.toString());
            return;
        }
        auto map = readTbmFile(*path);
        if (!map) {
            return;
        }
        set->originX = map->originX;
        set->originZ = map->originZ;
        set->texelSize = map->texelSize;
        set->width = map->width;
        set->height = map->height;
        set->indices = std::move(map->indices);
    });
    return set;
}

} // namespace world
