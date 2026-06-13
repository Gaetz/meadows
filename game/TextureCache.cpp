#include "game/TextureCache.hpp"

#include "engine/assets/Image.hpp"
#include "engine/core/Log.hpp"

namespace game {

rhi::TextureHandle TextureCache::resolve(const core::Guid& sprite) {
    if (!sprite.isValid()) {
        return {};
    }
    if (const auto it = byGuid.find(sprite); it != byGuid.end()) {
        return it->second;
    }

    // Cache the miss too (an invalid handle), so a missing asset is not
    // re-resolved every frame.
    const auto path = assets.resolve(sprite);
    if (!path) {
        LOG_WARN("TextureCache: no asset registered for sprite {}",
                 sprite.toString());
        byGuid.emplace(sprite, rhi::TextureHandle {});
        return {};
    }

    const auto image = assets::loadImageFile(*path);
    if (!image) {
        byGuid.emplace(sprite, rhi::TextureHandle {});
        return {};
    }

    const rhi::TextureHandle texture = device.createTexture(
        { .width = image->width,
          .height = image->height,
          .filter = rhi::FilterMode::Nearest },
        image->pixels.data());
    byGuid.emplace(sprite, texture);
    return texture;
}

void TextureCache::clear() {
    for (auto& [guid, texture] : byGuid) {
        if (texture.id != 0) {
            device.destroyTexture(texture);
        }
    }
    byGuid.clear();
}

} // namespace game
