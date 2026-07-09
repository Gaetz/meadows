#pragma once

#include <unordered_map>

#include "engine/assets/AssetDatabase.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Guid.hpp"
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/rhi/Device.hpp"

namespace game {

// The async-residency skeleton shared by TextureCache and MeshCache (audit
// U5-2 — it was hand-copied between the two, a bug fixed in one was not
// fixed in the other). Guid -> GPU payload, never blocking (§7):
//   - resolve() returns the payload to draw NOW — the real resource once
//     resident, a placeholder while a decode is in flight;
//   - a first sighting kicks a pure-CPU decode on the JobSystem; the worker
//     captures the shared completion queue (never `this` — the teardown-
//     safety pattern: a job outliving the cache pushes harmlessly);
//   - pumpUploads(), once per frame on the main thread, drains finished
//     decodes, uploads (GL is main-thread only) and flips entries;
//   - clear() bumps the generation so stale in-flight results are dropped
//     on arrival (cancellation) — a §5 re-resolution may remap any guid.
//
// Traits contract (state allowed — e.g. the texture upload desc):
//   using Payload;      // GPU-side entry state; resolve() returns const&
//   using DecodedData;  // the worker's output (optional<...> style)
//   static constexpr const char* kLabel;  // log prefix ("TextureCache")
//   static constexpr const char* kNoun;   // log noun ("sprite", "model")
//   Payload createPlaceholder(rhi::Device&);
//   void destroyPlaceholder(rhi::Device&, Payload&);
//   Payload makePending(const Payload& placeholder); // shown while decoding
//   Payload makeFailed(const Payload& placeholder);  // shown after failure
//   static DecodedData decode(const std::filesystem::path&); // WORKER thread
//   bool decoded(const DecodedData&);                 // upload-worthy?
//   Payload upload(rhi::Device&, DecodedData&&);      // main thread
//   void destroyPayload(rhi::Device&, Payload&);      // resident entries
template <typename Traits>
class ResidencyCache {
public:
    using Payload = typename Traits::Payload;
    using DecodedData = typename Traits::DecodedData;

    ResidencyCache(rhi::Device& device, const assets::AssetDatabase& assets,
                   core::JobSystem& jobs, Traits traitsIn = {})
        : device { device }, assets { assets }, jobs { jobs },
          traits_ { std::move(traitsIn) } {
        placeholder = traits_.createPlaceholder(device);
        invalid = traits_.makeFailed(placeholder);
    }
    ~ResidencyCache() {
        // No wait: in-flight decode jobs hold their own ref to `shared` and
        // never touch this cache, so they finish (and free `shared`) on
        // their own. We just release our GPU resources (main thread only).
        clear();
        traits_.destroyPlaceholder(device, placeholder);
    }
    ResidencyCache(const ResidencyCache&) = delete;
    ResidencyCache& operator=(const ResidencyCache&) = delete;

    // Main thread. The payload to draw NOW; never blocks. A first sighting
    // schedules the decode.
    const Payload& resolve(const core::Guid& guid) {
        if (!guid.isValid()) {
            return invalid;
        }
        if (const auto it = byGuid.find(guid); it != byGuid.end()) {
            return it->second.payload;
        }
        const auto path = assets.resolve(guid);
        if (!path) {
            LOG_WARN("{}: no asset registered for {} {}", Traits::kLabel,
                     Traits::kNoun, guid.toString());
            return byGuid
                .emplace(guid, Entry { traits_.makeFailed(placeholder),
                                       Residency::Failed })
                .first->second.payload;
        }
        // First sighting: show the placeholder, decode off-thread. The
        // worker touches only the path (pure file IO + parse) and the
        // shared queue.
        const auto it =
            byGuid
                .emplace(guid, Entry { traits_.makePending(placeholder),
                                       Residency::Pending })
                .first;
        ++pending;
        jobs.enqueue([sharedRef = shared, guid, gen = generation,
                      file = *path] {
            sharedRef->decoded.push({ guid, gen, Traits::decode(file) });
        });
        return it->second.payload;
    }

    // Main thread, once per frame at a fixed point: drains finished
    // decodes, uploads, flips entries. Returns how many became resident.
    u32 pumpUploads() {
        u32 becameResident = 0;
        shared->decoded.drain([&](Result&& result) {
            if (result.generation != generation) {
                return; // cancelled by a clear() since it was kicked
            }
            const auto it = byGuid.find(result.guid);
            if (it == byGuid.end() ||
                it->second.state != Residency::Pending) {
                return; // gone, or already resolved by an earlier arrival
            }
            --pending; // leaving Pending, one way or the other
            if (!traits_.decoded(result.data)) {
                it->second = Entry { traits_.makeFailed(placeholder),
                                     Residency::Failed };
                return;
            }
            it->second = Entry { traits_.upload(device,
                                                std::move(result.data)),
                                 Residency::Resident };
            ++becameResident;
        });
        return becameResident;
    }

    // How many sighted assets are still decoding. A loading gate polls this
    // to know when the visible set is ready (§7).
    u32 pendingCount() const { return pending; }

    void clear() {
        for (auto& [guid, entry] : byGuid) {
            // Only resident entries own GPU resources; the others hold the
            // shared placeholder (freed once, in the destructor).
            if (entry.state == Residency::Resident) {
                traits_.destroyPayload(device, entry.payload);
            }
        }
        byGuid.clear();
        pending = 0;
        ++generation; // stale in-flight results are dropped on arrival
    }

protected:
    // The payload of a RESIDENT entry, else nullptr (never triggers a
    // load) — derived accessors (MeshCache::cpuMesh) read through this.
    const Payload* residentPayload(const core::Guid& guid) const {
        const auto it = byGuid.find(guid);
        return it != byGuid.end() && it->second.state == Residency::Resident
                   ? &it->second.payload
                   : nullptr;
    }

    Traits& traits() { return traits_; }

private:
    enum class Residency { Pending, Resident, Failed };
    struct Entry {
        Payload payload;
        Residency state { Residency::Pending };
    };
    struct Result {
        core::Guid guid;
        u32 generation { 0 };
        DecodedData data;
    };
    // Outlives the cache: workers capture the shared_ptr, not `this`.
    struct Shared {
        core::ConcurrentQueue<Result> decoded;
    };

    rhi::Device& device;
    const assets::AssetDatabase& assets;
    core::JobSystem& jobs;
    Traits traits_ {};

    Payload placeholder {};
    Payload invalid {}; // what an invalid guid resolves to
    std::unordered_map<core::Guid, Entry> byGuid;
    sptr<Shared> shared { std::make_shared<Shared>() };
    u32 generation { 0 };
    u32 pending { 0 };
};

} // namespace game
