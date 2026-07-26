#pragma once

#include <optional>

#include "engine/assets/MeshData.hpp"
#include "engine/render/ResidencyCache.hpp"

namespace render {

// Resolves a model asset GUID to GPU vertex/index buffers — the consumer
// side of `RenderSnapshot.meshes` (contract: docs/HORIZONTAL-PASS.md).
// The async-residency machinery lives in ResidencyCache; this file keeps only the
// mesh specifics: the magenta placeholder box, the glTF decode + grounding,
// the buffer upload and the retained CPU geometry (collision cooking and
// editor picking read it). A failed/missing asset keeps the placeholder —
// a visible error beats an invisible prop.
struct MeshCacheTraits {
    struct Gpu {
        rhi::BufferHandle vertices {};
        rhi::BufferHandle indices {};
        u32 indexCount { 0 };
    };
    // CPU-side geometry retained for RESIDENT meshes, plus
    // the local-space bounds.
    struct CpuMesh {
        vector<Vec3> positions;
        vector<u32> indices;
        Vec3 boundsMin { 0.0f };
        Vec3 boundsMax { 0.0f };
    };
    struct Payload {
        Gpu gpu {};
        uptr<CpuMesh> cpu; // resident only
    };

    using DecodedData = std::optional<render::MeshData>;
    static constexpr const char* kLabel = "MeshCache";
    static constexpr const char* kNoun = "model";

    Payload createPlaceholder(rhi::Device& device);
    void destroyPlaceholder(rhi::Device& device, Payload& payload);
    // Both the pending AND the failed state show the placeholder box.
    Payload makePending(const Payload& placeholder) {
        return { placeholder.gpu, nullptr };
    }
    Payload makeFailed(const Payload& placeholder) {
        return { placeholder.gpu, nullptr };
    }
    static DecodedData decode(const std::filesystem::path& path);
    bool decoded(const DecodedData& data) {
        return data.has_value() && !data->indices.empty();
    }
    Payload upload(rhi::Device& device, DecodedData&& data);
    void destroyPayload(rhi::Device& device, Payload& payload);
};

class MeshCache : public ResidencyCache<MeshCacheTraits> {
public:
    using Gpu = MeshCacheTraits::Gpu;
    using CpuMesh = MeshCacheTraits::CpuMesh;

    MeshCache(rhi::Device& device, const assets::AssetDatabase& assets,
              core::JobSystem& jobs)
        : ResidencyCache(device, assets, jobs) {}

    // Main thread. The buffers to draw NOW: the real mesh once resident,
    // the placeholder box while decoding or after a failure.
    const Gpu& resolve(const core::Guid& model) {
        return ResidencyCache::resolve(model).gpu;
    }

    // The retained CPU geometry of a RESIDENT mesh; nullptr while pending,
    // failed or never sighted (does NOT trigger a load — resolve() does).
    const CpuMesh* cpuMesh(const core::Guid& model) const {
        const Payload* payload = residentPayload(model);
        return payload ? payload->cpu.get() : nullptr;
    }

    // A PROCEDURAL mesh (the sword) under a guid —
    // resident immediately, drawable through resolve() like any asset.
    void injectProcedural(const core::Guid& guid, render::MeshData mesh) {
        inject(guid, DecodedData { std::move(mesh) });
    }
};

} // namespace render
