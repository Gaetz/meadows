#include "engine/assets/CookedTexture.hpp"

#include <cstring>
#include <fstream>

#include "engine/core/Log.hpp"

namespace assets {

namespace {

constexpr u32 kMagic = 0x5845544Du; // 'MTEX' little-endian
constexpr u32 kVersion = 1;

// Stable on-disk codes: never renumber, only append. Decoupled from the
// rhi::TextureFormat enum so reordering it cannot corrupt cooked files.
constexpr u32 formatCode(rhi::TextureFormat f) {
    switch (f) {
    case rhi::TextureFormat::RGBA8:     return 1;
    case rhi::TextureFormat::SRGBA8:    return 2;
    case rhi::TextureFormat::BC7_SRGB:  return 3;
    case rhi::TextureFormat::BC7_UNORM: return 4;
    case rhi::TextureFormat::BC5_UNORM: return 5;
    case rhi::TextureFormat::R16_UNORM: return 6;
    default:                            return 0; // not a cookable format
    }
}

std::optional<rhi::TextureFormat> formatFromCode(u32 code) {
    switch (code) {
    case 1: return rhi::TextureFormat::RGBA8;
    case 2: return rhi::TextureFormat::SRGBA8;
    case 3: return rhi::TextureFormat::BC7_SRGB;
    case 4: return rhi::TextureFormat::BC7_UNORM;
    case 5: return rhi::TextureFormat::BC5_UNORM;
    case 6: return rhi::TextureFormat::R16_UNORM;
    default: return std::nullopt;
    }
}

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
    const auto format = formatFromCode(header.format);
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
    const u64 expected = rhi::textureDataBytes(tex.desc());
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
    const u32 code = formatCode(texture.format);
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
    file.write(reinterpret_cast<const char*>(texture.payload.data()),
               static_cast<std::streamsize>(texture.payload.size()));
    return static_cast<bool>(file);
}

} // namespace assets
