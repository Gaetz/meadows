#pragma once

#include <filesystem>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace assets {

// .mtex cooked-texture container: a POD header plus a payload whose byte
// layout is EXACTLY what rhi::Device::createTexture expects with
// pixelsIncludeMips — full mip chain, mip-major, layers contiguous inside
// each mip. Written by tools/cooker, read by the runtime loader and the
// tests; headless (only the rhi header PODs are involved, no device).
// On-disk format ids are stable codes independent of the rhi enum order.
struct CookedTexture {
    u32 width { 0 };
    u32 height { 0 };
    u32 arrayLayers { 1 };
    u32 mipLevels { 1 };
    rhi::TextureFormat format { rhi::TextureFormat::RGBA8 };
    vector<u8> payload; // textureDataBytes(desc()) bytes

    // The matching creation descriptor (sampled, trilinear-ready). Wrap
    // defaults to Repeat: every cooked texture so far is a tiling material.
    rhi::TextureDesc desc() const {
        return { .width = width,
                 .height = height,
                 .arrayLayers = arrayLayers,
                 .mipLevels = mipLevels,
                 .pixelsIncludeMips = true,
                 .format = format,
                 .filter = rhi::FilterMode::Linear,
                 .wrap = rhi::AddressMode::Repeat };
    }
};

// Synchronous read with full validation (magic, version, format code,
// payload size against rhi::textureDataBytes). Returns nullopt with a
// logged error on failure. Async loading lives in the streaming path (§7).
std::optional<CookedTexture> loadCookedTexture(
    const std::filesystem::path& path);

// Returns false with a logged error on failure (unencodable format, IO).
bool saveCookedTexture(const std::filesystem::path& path,
                       const CookedTexture& texture);

} // namespace assets
