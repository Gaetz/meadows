#include "world/terrain/TerrainRegions.hpp"

#include <cstring>
#include <fstream>
#include <type_traits>

#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "world/worldspace/WorldForms.hpp"

namespace world {

namespace {

// TRG3 appends the solved-water fields; TRG2 (story-terrain assets
// cooked before the water refonte) still reads, with empty water.
constexpr char kMagic[4] = { 'T', 'R', 'G', '3' };
constexpr char kMagicV2[4] = { 'T', 'R', 'G', '2' };
constexpr u32 kMaxSamples = 8192;

bool validWater(const render::TerrainRegion& region) {
    if (region.waterWidth == 0 && region.waterHeight == 0) {
        return region.waterSurface.empty() && region.waterDepth.empty() &&
               region.waterVelX.empty() && region.waterVelZ.empty() &&
               region.waterFlux.empty();
    }
    const size_t cells =
        static_cast<size_t>(region.waterWidth) * region.waterHeight;
    return region.waterWidth >= 2 && region.waterHeight >= 2 &&
           region.waterWidth <= kMaxSamples &&
           region.waterHeight <= kMaxSamples &&
           region.waterTexel > 0.0f &&
           region.waterSurface.size() == cells &&
           region.waterDepth.size() == cells &&
           region.waterVelX.size() == cells &&
           region.waterVelZ.size() == cells &&
           region.waterFlux.size() == cells;
}

bool validGrid(const render::TerrainRegion& region) {
    const size_t cells =
        static_cast<size_t>(region.width) * region.height;
    const size_t maskCells =
        static_cast<size_t>(region.maskWidth) * region.maskHeight;
    if (region.width < 2 || region.height < 2 ||
        region.width > kMaxSamples || region.height > kMaxSamples ||
        region.texelSize <= 0.0f || region.heights.size() != cells ||
        !validWater(region)) {
        return false;
    }
    const auto maskOk = [&](const vector<u8>& channel) {
        return channel.size() == maskCells;
    };
    if (maskCells == 0) {
        return region.detailAmp.empty() && region.flow.empty() &&
               region.wetness.empty() && region.beach.empty() &&
               region.biome.empty();
    }
    return region.maskWidth >= 2 && region.maskHeight >= 2 &&
           region.maskWidth <= kMaxSamples &&
           region.maskHeight <= kMaxSamples && maskOk(region.detailAmp) &&
           maskOk(region.flow) && maskOk(region.wetness) &&
           maskOk(region.beach) && maskOk(region.biome) &&
           (region.rockExposure.empty() || maskOk(region.rockExposure));
}

} // namespace

bool writeTrgFile(const std::filesystem::path& path,
                  const render::TerrainRegion& region) {
    if (!validGrid(region)) {
        LOG_ERROR("writeTrgFile: malformed region for {}", path.string());
        return false;
    }
    std::ofstream file { path, std::ios::binary | std::ios::trunc };
    if (!file) {
        LOG_ERROR("writeTrgFile: cannot open {}", path.string());
        return false;
    }
    const auto write = [&](const auto& value) {
        file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    file.write(kMagic, 4);
    write(region.width);
    write(region.height);
    write(region.originX);
    write(region.originZ);
    write(region.texelSize);
    write(region.edgeBlend);
    file.write(reinterpret_cast<const char*>(region.heights.data()),
               static_cast<std::streamsize>(region.heights.size() *
                                            sizeof(f32)));
    write(region.maskWidth);
    write(region.maskHeight);
    const auto writeChannel = [&](const vector<u8>& channel) {
        file.write(reinterpret_cast<const char*>(channel.data()),
                   static_cast<std::streamsize>(channel.size()));
    };
    writeChannel(region.detailAmp);
    writeChannel(region.flow);
    writeChannel(region.wetness);
    writeChannel(region.beach);
    writeChannel(region.biome);
    if (region.rockExposure.empty() && region.maskWidth > 0) {
        // The channel is optional in memory but fixed in the format.
        const vector<u8> zeros(static_cast<size_t>(region.maskWidth) *
                                   region.maskHeight,
                               0);
        writeChannel(zeros);
    } else {
        writeChannel(region.rockExposure);
    }
    write(region.waterWidth);
    write(region.waterHeight);
    write(region.waterTexel);
    const auto writeRaw = [&](const auto& field) {
        file.write(
            reinterpret_cast<const char*>(field.data()),
            static_cast<std::streamsize>(
                field.size() * sizeof(typename std::remove_reference_t<
                                      decltype(field)>::value_type)));
    };
    writeRaw(region.waterSurface);
    writeRaw(region.waterDepth);
    writeRaw(region.waterVelX);
    writeRaw(region.waterVelZ);
    writeRaw(region.waterFlux);
    return static_cast<bool>(file);
}

std::optional<render::TerrainRegion> readTrgFile(
    const std::filesystem::path& path) {
    std::ifstream file { path, std::ios::binary };
    if (!file) {
        LOG_ERROR("readTrgFile: cannot open {}", path.string());
        return std::nullopt;
    }
    char magic[4] = {};
    render::TerrainRegion region;
    const auto read = [&](auto& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
    };
    file.read(magic, 4);
    const bool v3 = std::memcmp(magic, kMagic, 4) == 0;
    const bool v2 = std::memcmp(magic, kMagicV2, 4) == 0;
    read(region.width);
    read(region.height);
    read(region.originX);
    read(region.originZ);
    read(region.texelSize);
    read(region.edgeBlend);
    if (!file || (!v3 && !v2) || region.width < 2 ||
        region.height < 2 || region.width > kMaxSamples ||
        region.height > kMaxSamples || region.texelSize <= 0.0f) {
        LOG_ERROR("readTrgFile: not a TRG region: {}", path.string());
        return std::nullopt;
    }
    region.heights.resize(static_cast<size_t>(region.width) *
                          region.height);
    file.read(reinterpret_cast<char*>(region.heights.data()),
              static_cast<std::streamsize>(region.heights.size() *
                                           sizeof(f32)));
    read(region.maskWidth);
    read(region.maskHeight);
    if (!file) {
        LOG_ERROR("readTrgFile: truncated region: {}", path.string());
        return std::nullopt;
    }
    if (region.maskWidth != 0 || region.maskHeight != 0) {
        if (region.maskWidth < 2 || region.maskHeight < 2 ||
            region.maskWidth > kMaxSamples ||
            region.maskHeight > kMaxSamples) {
            LOG_ERROR("readTrgFile: bad mask dims: {}", path.string());
            return std::nullopt;
        }
        const size_t maskCells =
            static_cast<size_t>(region.maskWidth) * region.maskHeight;
        const auto readChannel = [&](vector<u8>& channel) {
            channel.resize(maskCells);
            file.read(reinterpret_cast<char*>(channel.data()),
                      static_cast<std::streamsize>(maskCells));
        };
        readChannel(region.detailAmp);
        readChannel(region.flow);
        readChannel(region.wetness);
        readChannel(region.beach);
        readChannel(region.biome);
        readChannel(region.rockExposure);
        if (!file) {
            LOG_ERROR("readTrgFile: truncated masks: {}", path.string());
            return std::nullopt;
        }
    }
    if (v3) {
        read(region.waterWidth);
        read(region.waterHeight);
        read(region.waterTexel);
        if (!file) {
            LOG_ERROR("readTrgFile: truncated water header: {}",
                      path.string());
            return std::nullopt;
        }
        if (region.waterWidth != 0 || region.waterHeight != 0) {
            if (region.waterWidth < 2 || region.waterHeight < 2 ||
                region.waterWidth > kMaxSamples ||
                region.waterHeight > kMaxSamples ||
                region.waterTexel <= 0.0f) {
                LOG_ERROR("readTrgFile: bad water dims: {}",
                          path.string());
                return std::nullopt;
            }
            const size_t wcells = static_cast<size_t>(region.waterWidth) *
                                  region.waterHeight;
            const auto readRaw = [&](auto& field) {
                field.resize(wcells);
                file.read(
                    reinterpret_cast<char*>(field.data()),
                    static_cast<std::streamsize>(
                        wcells *
                        sizeof(typename std::remove_reference_t<
                               decltype(field)>::value_type)));
            };
            readRaw(region.waterSurface);
            readRaw(region.waterDepth);
            readRaw(region.waterVelX);
            readRaw(region.waterVelZ);
            readRaw(region.waterFlux);
            if (!file) {
                LOG_ERROR("readTrgFile: truncated water: {}",
                          path.string());
                return std::nullopt;
            }
        }
    }
    return region;
}

sptr<const render::TerrainBase> buildTerrainBase(
    const data::FormDatabase& forms, const assets::AssetDatabase& assets) {
    auto base = std::make_shared<render::TerrainBase>();
    data::forEach<TerrainRegionForm>(
        forms, [&](const TerrainRegionForm& form) {
            const auto path = assets.resolve(form.asset);
            if (!path) {
                LOG_WARN("TerrainRegion '{}': asset {} not registered",
                         form.displayName, form.asset.toString());
                return;
            }
            auto region = readTrgFile(*path);
            if (!region) {
                return;
            }
            region->detailAmplitude = form.detailAmplitude;
            region->detailWavelength = form.detailWavelength;
            region->detailOctaves = form.detailOctaves;
            base->regions.push_back(std::move(*region));
        });
    return base;
}

} // namespace world
