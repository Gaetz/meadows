#pragma once

#include <unordered_map>

#include "engine/assets/AssetDatabase.hpp"
#include "engine/core/Guid.hpp"
#include "engine/rhi/Device.hpp"

namespace game {

// Resolves a sprite asset GUID to a GPU texture, caching the result and owning
// the textures it creates (destroyed on clear() and on teardown). After a §5
// re-resolution an asset GUID may point at a different file, so the game clears
// the cache before re-spawning.
//
// Phase 2: synchronous load (small eager world). The async-residency path
// (§7: background decode → main-thread upload, placeholder while pending) lands
// with multithreading (Phase 4.5) / streaming (Phase 5). For now a missing or
// unloadable asset returns an invalid handle, which the SpriteRenderer draws
// with its white fallback.
class TextureCache {
public:
    TextureCache(rhi::Device& device, const assets::AssetDatabase& assets)
        : device { device }, assets { assets } {}
    ~TextureCache() { clear(); }

    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    rhi::TextureHandle resolve(const core::Guid& sprite);

    void clear();

private:
    rhi::Device& device;
    const assets::AssetDatabase& assets;
    std::unordered_map<core::Guid, rhi::TextureHandle> byGuid;
};

} // namespace game
