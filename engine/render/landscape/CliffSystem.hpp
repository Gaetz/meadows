#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/Frustum.hpp"
#include "engine/assets/MeshData.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"
#include "engine/rhi/UniqueHandle.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Continuous cliff-wall ribbons (docs/CLIFFS.md étage 2): one draped
// mesh per baked cliff band (TerrainRegion::cliffBands), GLUED to the
// sharpened heightfield — rows sample the real terrain along each
// node's fall line and push outward by a strata/noise relief, so the
// wall reads as ONE rock face instead of scattered slabs. Material =
// the terrain splat cliff layer, triplanar (cliff.frag), sharing the
// terrain pass's bind groups — the wall and the ground around it can
// never diverge in texture. Meshes rebuild when the terrain base
// republishes; each band keeps its own AABB for frustum culling.
class CliffSystem {
public:
    // Visible wall cap: taller faces keep their upper ground bare (the
    // sharpened heightfield still carries the silhouette).
    static constexpr f32 kMaxWallHeight = 60.0f;

    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Rebuilds the region meshes when params.base changed (publish).
    // Cheap when nothing moved; the build is main-thread (a few tens of
    // thousands of vertices per tile at most).
    void update(rhi::Device& device, const TerrainParams& params);

    // Main opaque pass, right after the terrain (same groups: 0 frame,
    // 1 splat, 2 shadow receivers).
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle splatBindGroup,
              rhi::BindGroupHandle shadowBindGroup,
              const Frustum* frustum = nullptr);

    // Depth-only caster pass (terrain caster shader — plain mesh).
    void drawDepth(rhi::CommandBuffer& cmd,
                   rhi::BindGroupHandle casterBindGroup,
                   const Frustum* frustum = nullptr);

    u32 wallCount() const { return walls; }
    u32 indicesThisFrame() const { return frameIndices; }
    void beginFrame() { frameIndices = 0; }

private:
    struct BandRange {
        u32 firstIndex { 0 };
        u32 indexCount { 0 };
        Vec3 lo {};
        Vec3 hi {};
    };
    struct RegionMesh {
        rhi::UniqueBuffer vertexBuffer;
        rhi::UniqueBuffer indexBuffer;
        vector<BandRange> ranges;
    };

    void buildMeshes(rhi::Device& device, const TerrainParams& params);
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void buildCasterPipeline(rhi::Device& device, ShaderLibrary& shaders);

    vector<RegionMesh> meshes;
    const void* baseKey { nullptr };
    u32 walls { 0 };
    u32 frameIndices { 0 };
    rhi::UniquePipeline pipeline;
    rhi::UniquePipeline casterPipeline;
    u64 shaderGeneration { 0 };
    u64 casterShaderGeneration { 0 };
};

} // namespace render
