#include "engine/render/TextureCache.hpp"

namespace render {

// A distinctive "still loading" marker (magenta/dark checker), so a pending
// asset reads differently from a missing one (the renderer's white fallback).
// Tiny and built-in — it never ships; it just makes the async path visible.
rhi::TextureHandle TextureCacheTraits::createPlaceholder(rhi::Device& device) {
    constexpr u32 size = 8;
    vector<u32> pixels(size * size);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const bool even = ((x / 4 + y / 4) % 2) == 0;
            pixels[y * size + x] = even ? 0xFFFF00FF : 0xFF400040; // ABGR
        }
    }
    return device.createTexture({ .width = size, .height = size },
                                pixels.data());
}

void TextureCacheTraits::destroyPlaceholder(rhi::Device& device,
                                            Payload& handle) {
    if (handle.id != 0) {
        device.destroyTexture(handle);
    }
}

// WORKER thread: pure file IO + decode — no GPU, no cache state.
TextureCacheTraits::DecodedData
TextureCacheTraits::decode(const std::filesystem::path& path) {
    return assets::loadImageFile(path);
}

// Main thread (the GL context is single-threaded).
rhi::TextureHandle TextureCacheTraits::upload(rhi::Device& device,
                                              DecodedData&& data) {
    return device.createTexture({ .width = data->width,
                                  .height = data->height,
                                  .format = desc.format,
                                  .filter = desc.filter },
                                data->pixels.data());
}

void TextureCacheTraits::destroyPayload(rhi::Device& device,
                                        Payload& handle) {
    if (handle.id != 0) {
        device.destroyTexture(handle);
    }
}

} // namespace render
