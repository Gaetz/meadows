#include "engine/assets/CookedTexture.hpp"

#include <cstring>
#include <fstream>

#include "engine/core/Log.hpp"

namespace assets {

namespace {

constexpr u32 kMagic = 0x5845544Du; // 'MTEX' little-endian
constexpr u32 kVersion = 2; // v2: per-layer average colors after the header

struct Header {
    u32 magic { 0 };
    u32 version { 0 };
    u32 format { 0 };
    u32 width { 0 };
    u32 height { 0 };
    u32 arrayLayers { 0 };
    u32 mipLevels { 0 };
};

} // namespace

std::optional<CookedTexture> loadCookedTexture(
    const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        LOG_ERROR("CookedTexture: cannot open '{}'", path.string());
        return std::nullopt;
    }
    Header header {};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || header.magic != kMagic) {
        LOG_ERROR("CookedTexture: '{}' is not a .mtex file", path.string());
        return std::nullopt;
    }
    if (header.version != kVersion) {
        LOG_ERROR("CookedTexture: '{}' has version {} (expected {}) — recook",
                  path.string(), header.version, kVersion);
        return std::nullopt;
    }
    const auto format = rhi::formatFromCookCode(header.format);
    if (!format || header.width == 0 || header.height == 0 ||
        header.arrayLayers == 0 || header.mipLevels == 0) {
        LOG_ERROR("CookedTexture: '{}' has an invalid header", path.string());
        return std::nullopt;
    }

    CookedTexture tex { .width = header.width,
                        .height = header.height,
                        .arrayLayers = header.arrayLayers,
                        .mipLevels = header.mipLevels,
                        .format = *format };
    tex.layerAverages.resize(header.arrayLayers, 0xff808080u);
    file.read(reinterpret_cast<char*>(tex.layerAverages.data()),
              static_cast<std::streamsize>(header.arrayLayers *
                                           sizeof(u32)));
    const u64 expected = rhi::textureDataBytes(tex.desc());
    constexpr u64 kMaxPayloadBytes = 1ull << 30; // sanity bound, not a design limit
    if (expected == 0 || expected > kMaxPayloadBytes) {
        LOG_ERROR("CookedTexture: '{}' has an implausible payload size ({})",
                  path.string(), expected);
        return std::nullopt;
    }
    tex.payload.resize(expected);
    file.read(reinterpret_cast<char*>(tex.payload.data()),
              static_cast<std::streamsize>(expected));
    if (!file || file.gcount() != static_cast<std::streamsize>(expected)) {
        LOG_ERROR("CookedTexture: '{}' payload truncated (expected {} bytes)",
                  path.string(), expected);
        return std::nullopt;
    }
    return tex;
}

bool saveCookedTexture(const std::filesystem::path& path,
                       const CookedTexture& texture) {
    const u32 code = rhi::formatCookCode(texture.format);
    if (code == 0) {
        LOG_ERROR("CookedTexture: format not cookable for '{}'",
                  path.string());
        return false;
    }
    const u64 expected = rhi::textureDataBytes(texture.desc());
    if (texture.payload.size() != expected) {
        LOG_ERROR("CookedTexture: '{}' payload is {} bytes, layout needs {}",
                  path.string(), texture.payload.size(), expected);
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        LOG_ERROR("CookedTexture: cannot write '{}'", path.string());
        return false;
    }
    const Header header { .magic = kMagic,
                          .version = kVersion,
                          .format = code,
                          .width = texture.width,
                          .height = texture.height,
                          .arrayLayers = texture.arrayLayers,
                          .mipLevels = texture.mipLevels };
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    vector<u32> averages = texture.layerAverages;
    averages.resize(texture.arrayLayers, 0xff808080u);
    file.write(reinterpret_cast<const char*>(averages.data()),
               static_cast<std::streamsize>(averages.size() * sizeof(u32)));
    file.write(reinterpret_cast<const char*>(texture.payload.data()),
               static_cast<std::streamsize>(texture.payload.size()));
    return static_cast<bool>(file);
}

} // namespace assets
