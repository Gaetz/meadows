#pragma once

#include <optional>
#include <unordered_map>

#include "engine/assets/AssetDatabase.hpp"
#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Guid.hpp"
#include "engine/assets/MeshData.hpp"
#include "engine/rhi/Device.hpp"

namespace core {
class JobSystem;
}

namespace game {

// Resolves a model asset GUID to GPU vertex/index buffers — the consumer
// side of `RenderSnapshot.meshes` (the H8 contract). Mirrors TextureCache:
// async residency (§7), resolve() NEVER blocks. A first sighting returns a
// placeholder box (magenta, like the texture checker: "loading" must read
// differently from "done") and kicks a background glTF decode on the
// JobSystem; pumpUploads(), once per frame on the main thread, uploads
// finished meshes and flips entries to resident. A failed/missing asset
// keeps the placeholder — a visible error beats an invisible prop.
class MeshCache {
public:
    struct Gpu {
        rhi::BufferHandle vertices {};
        rhi::BufferHandle indices {};
        u32 indexCount { 0 };
    };

    // CPU-side geometry retained for RESIDENT meshes (chantier 2 B2):
    // collision cooking (addStaticMesh) and editor picking read it. Also
    // carries the local-space bounds.
    struct CpuMesh {
        vector<Vec3> positions;
        vector<u32> indices;
        Vec3 boundsMin { 0.0f };
        Vec3 boundsMax { 0.0f };
    };

    MeshCache(rhi::Device& device, const assets::AssetDatabase& assets,
              core::JobSystem& jobs);
    ~MeshCache();

    MeshCache(const MeshCache&) = delete;
    MeshCache& operator=(const MeshCache&) = delete;

    // Main thread. The buffers to draw NOW: the real mesh once resident,
    // the placeholder box while decoding or after a failure.
    const Gpu& resolve(const core::Guid& model);

    // Main thread, once per frame: drains finished decodes, uploads them,
    // flips entries to resident. Returns how many became resident.
    u32 pumpUploads();

    // The retained CPU geometry of a RESIDENT mesh; nullptr while pending,
    // failed or never sighted (does NOT trigger a load — resolve() does).
    const CpuMesh* cpuMesh(const core::Guid& model) const;

    u32 pendingCount() const { return pending; }

    void clear();

private:
    struct Decoded {
        core::Guid model;
        u32 generation { 0 };
        std::optional<render::MeshData> mesh;
    };

    // Outlives the cache (workers capture the shared_ptr, not `this`) —
    // same teardown story as TextureCache.
    struct Shared {
        core::ConcurrentQueue<Decoded> decoded;
    };

    enum class Residency { Pending, Resident, Failed };
    struct Entry {
        Gpu gpu {}; // the placeholder while Pending/Failed
        Residency state { Residency::Pending };
        uptr<CpuMesh> cpu; // resident only
    };

    rhi::Device& device;
    const assets::AssetDatabase& assets;
    core::JobSystem& jobs;

    Gpu placeholder {};
    std::unordered_map<core::Guid, Entry> byGuid;
    sptr<Shared> shared { std::make_shared<Shared>() };
    u32 generation { 0 };
    u32 pending { 0 };
};

} // namespace game
