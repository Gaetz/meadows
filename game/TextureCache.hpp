#pragma once

#include <optional>

#include "engine/assets/Image.hpp"
#include "game/ResidencyCache.hpp"

namespace game {

// Resolves a sprite asset GUID to a GPU texture, caching the result and owning
// the textures it creates (destroyed on clear() and on teardown). After a §5
// re-resolution an asset GUID may point at a different file, so the game clears
// the cache before re-spawning.
//
// The async-residency machinery (worker decode -> queue -> main-thread
// upload, §7 / §9 Phase 5) lives in ResidencyCache; this file
// keeps only what is texture-specific: the checker placeholder, the image
// decode, the upload description and the white fallback on failure.
struct TextureCacheTraits {
    // How uploaded textures are created. Defaults fit the 2D sprite path
    // (linear RGBA8, nearest); the 3D material path passes SRGBA8 + Linear
    // (albedo is authored in sRGB and sampled smoothly on meshes).
    struct UploadDesc {
        rhi::TextureFormat format { rhi::TextureFormat::RGBA8 };
        rhi::FilterMode filter { rhi::FilterMode::Nearest };
    };

    using Payload = rhi::TextureHandle;
    using DecodedData = std::optional<assets::Image>;
    static constexpr const char* kLabel = "TextureCache";
    static constexpr const char* kNoun = "sprite";

    UploadDesc desc {};

    Payload createPlaceholder(rhi::Device& device);
    void destroyPlaceholder(rhi::Device& device, Payload& handle);
    // Pending shows the checker; failure falls back to an invalid handle
    // (the renderer's white fallback — "missing" reads differently from
    // "loading").
    Payload makePending(const Payload& placeholder) { return placeholder; }
    Payload makeFailed(const Payload&) { return {}; }
    static DecodedData decode(const std::filesystem::path& path);
    bool decoded(const DecodedData& data) { return data.has_value(); }
    Payload upload(rhi::Device& device, DecodedData&& data);
    void destroyPayload(rhi::Device& device, Payload& handle);
};

class TextureCache : public ResidencyCache<TextureCacheTraits> {
public:
    using UploadDesc = TextureCacheTraits::UploadDesc;

    TextureCache(rhi::Device& device, const assets::AssetDatabase& assets,
                 core::JobSystem& jobs, UploadDesc upload = {})
        : ResidencyCache(device, assets, jobs,
                         TextureCacheTraits { .desc = upload }) {}
};

} // namespace game
