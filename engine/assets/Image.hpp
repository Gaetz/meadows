#pragma once

#include <filesystem>
#include <optional>

#include "engine/core/Defines.hpp"

namespace assets {

// Decoded RGBA8 image, ready for rhi::Device::createTexture.
struct Image {
    u32 width { 0 };
    u32 height { 0 };
    vector<u8> pixels; // width * height * 4, row-major, top-left origin
};

// Synchronous decode via stb_image (PNG and friends). Returns nullopt with
// a logged error on failure. Async loading lives in the streaming path (§7).
std::optional<Image> loadImageFile(const std::filesystem::path& path);

} // namespace assets
