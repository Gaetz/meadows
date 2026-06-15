#pragma once

#include <optional>
#include <unordered_map>

#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/Image.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Guid.hpp"
#include "engine/rhi/Device.hpp"

namespace core {
class JobSystem;
}

namespace game {

// Resolves a sprite asset GUID to a GPU texture, caching the result and owning
// the textures it creates (destroyed on clear() and on teardown). After a §5
// re-resolution an asset GUID may point at a different file, so the game clears
// the cache before re-spawning.
//
// Async residency (§7, §9 Phase 4.5): resolve() NEVER blocks. A first sighting
// returns a placeholder and kicks a background decode on the JobSystem; the
// worker decodes CPU-side (touching neither the GPU nor this cache) and pushes
// the result into a ConcurrentQueue. pumpUploads(), called once per frame on the
// main thread, drains finished decodes and uploads them to the GPU (GL is
// main-thread only), flipping each handle from placeholder to resident. This is
// the first place JobSystem + ConcurrentQueue + the snapshot seam collaborate.
class TextureCache {
public:
    TextureCache(rhi::Device& device, const assets::AssetDatabase& assets,
                 core::JobSystem& jobs);
    ~TextureCache();

    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // Main thread. The handle to draw NOW: the real texture once resident, the
    // placeholder while a decode is in flight, or an invalid handle (white
    // fallback) for a missing/unreadable asset. A first sighting schedules the
    // decode; it never blocks the caller.
    rhi::TextureHandle resolve(const core::Guid& sprite);

    // Main thread, once per frame at a fixed point: drains finished decodes,
    // uploads them to the GPU, and flips handles to resident. Returns how many
    // became resident this call.
    u32 pumpUploads();

    // How many sighted assets are still decoding (not yet resident or failed).
    // A loading gate polls this to know when the visible set is ready (§7).
    u32 pendingCount() const { return pending; }

    void clear();

private:
    // A worker's output: the decoded image (nullopt on failure), tagged with the
    // guid and the cache generation it was kicked under (for cancellation).
    struct Decoded {
        core::Guid sprite;
        u32 generation { 0 };
        std::optional<assets::Image> image;
    };

    // The completion queue, kept in a heap block shared with in-flight jobs so it
    // OUTLIVES the cache. A worker captures the shared_ptr (not `this`), so when
    // the cache is destroyed mid-decode the worker still pushes here harmlessly
    // instead of touching freed members — no join/wait at teardown, and so no
    // race tearing a condition variable down while a worker is mid-notify.
    struct Shared {
        core::ConcurrentQueue<Decoded> decoded;
    };

    enum class Residency { Pending, Resident, Failed };
    struct Entry {
        rhi::TextureHandle texture {}; // the placeholder while Pending
        Residency state { Residency::Pending };
    };

    rhi::Device& device;
    const assets::AssetDatabase& assets;
    core::JobSystem& jobs;

    rhi::TextureHandle placeholder {};
    std::unordered_map<core::Guid, Entry> byGuid;
    sptr<Shared> shared { std::make_shared<Shared>() };
    u32 generation { 0 }; // bumped on clear(); stale results are dropped
    u32 pending { 0 };    // entries currently in the Pending state
};

} // namespace game
