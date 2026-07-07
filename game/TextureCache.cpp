#include "game/TextureCache.hpp"

#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"

namespace game {

namespace {
// A distinctive "still loading" marker (magenta/dark checker), so a pending
// asset reads differently from a missing one (the renderer's white fallback).
// Tiny and built-in — it never ships; it just makes the async path visible.
rhi::TextureHandle createPlaceholderTexture(rhi::Device& device) {
    constexpr u32 size = 8;
    vector<u32> pixels(size * size);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const bool even = ((x / 4 + y / 4) % 2) == 0;
            pixels[y * size + x] = even ? 0xFFFF00FF : 0xFF400040; // ABGR magenta
        }
    }
    return device.createTexture({ .width = size, .height = size }, pixels.data());
}
} // namespace

TextureCache::TextureCache(rhi::Device& device,
                           const assets::AssetDatabase& assets,
                           core::JobSystem& jobs, UploadDesc upload)
    : device { device }, assets { assets }, jobs { jobs }, upload { upload } {
    placeholder = createPlaceholderTexture(device);
}

TextureCache::~TextureCache() {
    // No wait: in-flight decode jobs hold their own ref to `shared` and never
    // touch this cache, so they finish (and free `shared`) on their own. We just
    // release our GPU resources, which only the main thread ever owns.
    clear();
    if (placeholder.id != 0) {
        device.destroyTexture(placeholder);
    }
}

rhi::TextureHandle TextureCache::resolve(const core::Guid& sprite) {
    if (!sprite.isValid()) {
        return {};
    }
    if (const auto it = byGuid.find(sprite); it != byGuid.end()) {
        return it->second.texture;
    }

    const auto path = assets.resolve(sprite);
    if (!path) {
        LOG_WARN("TextureCache: no asset registered for sprite {}",
                 sprite.toString());
        byGuid.emplace(sprite, Entry { {}, Residency::Failed });
        return {};
    }

    // First sighting: show the placeholder and decode off-thread. The worker
    // captures `shared` (not `this`) and touches neither the GPU nor `byGuid` —
    // only the path (pure file IO + decode) and the shared completion queue.
    byGuid.emplace(sprite, Entry { placeholder, Residency::Pending });
    ++pending;
    jobs.enqueue([shared = shared, sprite, gen = generation, path = *path] {
        shared->decoded.push(Decoded { sprite, gen, assets::loadImageFile(path) });
    });
    return placeholder;
}

u32 TextureCache::pumpUploads() {
    u32 becameResident = 0;
    shared->decoded.drain([&](Decoded&& result) {
        // Dropped by a clear() since it was kicked: discard (cancellation).
        if (result.generation != generation) {
            return;
        }
        const auto it = byGuid.find(result.sprite);
        if (it == byGuid.end() || it->second.state != Residency::Pending) {
            return; // gone, or already resolved by an earlier arrival
        }
        --pending; // leaving Pending, one way or the other
        if (!result.image) {
            it->second = Entry { {}, Residency::Failed }; // white fallback
            return;
        }
        // GPU upload on the main thread (the GL context is single-threaded).
        const rhi::TextureHandle texture = device.createTexture(
            { .width = result.image->width,
              .height = result.image->height,
              .format = upload.format,
              .filter = upload.filter },
            result.image->pixels.data());
        it->second = Entry { texture, Residency::Resident };
        ++becameResident;
    });
    return becameResident;
}

void TextureCache::clear() {
    for (auto& [guid, entry] : byGuid) {
        // Only resident entries own a texture; pending ones hold the shared
        // placeholder (freed once, in the destructor), failed ones hold none.
        if (entry.state == Residency::Resident && entry.texture.id != 0) {
            device.destroyTexture(entry.texture);
        }
    }
    byGuid.clear();
    pending = 0; // dropped entries; stale in-flight results bounce off generation
    // Bump the generation so a decode still in flight is dropped on arrival
    // rather than applied to the freshly re-resolved world (cancellation).
    ++generation;
}

} // namespace game
