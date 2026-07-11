#pragma once

#include <memory>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/GpuProbe.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace core {
class JobSystem;
}
namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Which indirect-lighting technique the surface shaders use (chantier RC,
// docs/RADIANCE-CASCADES.md). Classic = the current flat sky ambient ×
// terrain light map; RadianceCascades = the GI volume (gi.glsl, G6) with
// Classic as its far-field fallback. Runtime-switchable from the render
// panel — the parallel-technique seam the dev asked for (modding later).
enum class GiTechnique : u32 {
    Classic,
    RadianceCascades,
};

// Every cost-affecting parameter lives HERE and in the UI (dev workflow:
// quality first, HE does the perf descent with these knobs — never
// constants). Structural fields recreate the volumes when applied values
// change (the reflectionScale pattern).
struct RcTuning {
    // Structural (recreate):
    i32 resolution { 64 };    // voxels per axis, both clips
    f32 fineVoxel { 0.5f };   // meters — interiors / near detail
    f32 coarseVoxel { 2.0f }; // meters — mid-field (span = res × voxel)
    // Live:
    GiTechnique technique { GiTechnique::Classic }; // apply switch (G6)
    f32 intensity { 1.0f };   // indirect strength at apply (G6)
    f32 skyFactor { 0.5f };   // sky ambient folded into injected surfaces
    i32 updateInterval { 1 }; // inject every N frames (1 = every frame)
    i32 debugView { 0 };      // 0 off, 1 = fine clip, 2 = coarse clip
};

// Radiance cascades GI — G2 scope: the camera-centered voxel CLIPMAP
// (2 levels) fed by an analytic terrain height/albedo tile (worker-baked,
// the TerrainLightMap pattern — patch/sculpt-aware since terrain::height
// runs on the CPU) + per-voxel sun via the CSM, re-injected in compute
// every frame (single-shot: fully dynamic, zero invalidation). Cascades
// (G4), merge (G5) and the gi.glsl apply (G6) build on these volumes.
class RadianceCascades {
public:
    static constexpr u32 kTileSize = 256; // height/albedo bake tile (texels)

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobs);
    void destroy(rhi::Device& device);
    void refreshPipelines(rhi::Device& device, ShaderLibrary& shaders);

    // Main thread, once per frame, OUTSIDE any render pass (compute):
    // pumps the tile bake, recreates volumes on knob changes, uploads the
    // RC UBO and dispatches the injection. Requires the frame UBO already
    // uploaded and the CSM rendered (the inject samples uShadowMap).
    // `terrainLightGroup` may be null (far-sun falls back to the CSM only).
    void update(rhi::Device& device, rhi::CommandBuffer& cmd,
                const TerrainParams& params, const Vec3& cameraPos,
                rhi::BindGroupHandle frameBindGroup,
                rhi::BindGroupHandle shadowBindGroup,
                rhi::BindGroupHandle terrainLightGroup,
                rhi::Device* probeDevice = nullptr,
                GpuProbe* probe = nullptr);

    // Fullscreen raymarch of the selected clip volume (tuning.debugView),
    // recorded INSIDE the composite pass, after the tonemap draw.
    void drawDebug(rhi::CommandBuffer& cmd,
                   rhi::BindGroupHandle frameBindGroup);

    bool ready() const { return tileUploaded && clipFine.id() != 0; }
    RcTuning tuning;

private:
    void createVolumes(rhi::Device& device);
    void pumpTileBake(rhi::Device& device, const TerrainParams& params,
                      const Vec3& cameraPos);

    struct BakedTile {
        vector<f32> height; // kTileSize², meters
        vector<u8> albedo;  // kTileSize², RGBA8 (a unused)
        Vec2 center {};
        u64 gen { 0 };
    };

    core::JobSystem* jobs { nullptr };
    std::shared_ptr<core::ConcurrentQueue<BakedTile>> baked;
    rhi::UniqueTexture heightTex;  // R32F — GPU-side terrain height
    rhi::UniqueTexture albedoTex;  // RGBA8 — material proxy colors
    rhi::UniqueSampler tileSampler;
    rhi::UniqueSampler volumeSampler;
    Vec2 tileCenter {};
    f32 tileSpan { 0.0f };
    bool tileInFlight { false };
    bool tileUploaded { false };
    u64 tileGeneration { 0 };

    rhi::UniqueTexture clipFine;   // res³ RGBA16F: rgb radiance, a occupancy
    rhi::UniqueTexture clipCoarse;
    rhi::UniqueBuffer rcUbo;
    rhi::UniqueBindGroup injectGroup;
    rhi::UniqueBindGroup debugGroup;
    rhi::UniquePipeline injectPipeline;
    u64 injectGeneration { 0 };
    rhi::UniquePipeline debugPipeline;
    u64 debugGeneration { 0 };

    // Applied structural knobs (recreate when the tuning diverges).
    i32 appliedResolution { 0 };
    f32 appliedFineVoxel { 0.0f };
    f32 appliedCoarseVoxel { 0.0f };
    u32 frameCounter { 0 };
};

} // namespace render
