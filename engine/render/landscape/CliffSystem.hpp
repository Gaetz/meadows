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

// One planned rock BLOCK (docs/CLIFFS.md étage 2, blocks rework —
// retour dev 2026-08-06): a deformed parallelepiped INSERTED into the
// mountainside so its faces BECOME the cliff surface — the Dziura
// "blocking → detailed stone" made automatic. Near-vertical fronts
// (small backward lean), flat-ish ledge tops, stacked in terraces up
// tall faces — real stepped verticality instead of a skin draped on
// the diagonal slope. Local frame: x along the band, y up, z outward;
// world = base + R_y(yaw) * R_x(-lean) * local.
struct CliffBlock {
    Vec3 base;      // front-bottom-center, world
    f32 width { 12.0f };
    f32 height { 12.0f };
    f32 depth { 9.0f };
    f32 yaw { 0.0f };
    f32 lean { 0.1f }; // radians, top tips INTO the hill
    // De-cubing warp, CONTINUOUS over the local box so the face sheets
    // stay welded at the edges: the top shrinks (taper), the whole
    // block shears sideways/backward (skew), and a small roll tips it
    // off the vertical — no two blocks read as the same brick.
    f32 taper { 0.15f };
    f32 skewX { 0.0f };
    f32 skewZ { 0.0f };
    f32 roll { 0.0f };
    u32 seed { 0 };
};

// Deterministic per (params, band, regionSeed): the region mesh build
// AND the collision ring derive the SAME blocks from the baked band
// polylines.
u32 cliffRegionSeed(const TerrainRegion& region);
vector<CliffBlock> planCliffBlocks(const TerrainParams& params,
                                   const CliffBand& band, u32 regionSeed);

// Renders the planned blocks: one merged mesh pair per region (near =
// displaced strata faces, far = plain boxes), per-band ranges with
// AABBs for frustum + distance pick. Material = the terrain splat
// cliff layer, triplanar (cliff.frag), sharing the terrain pass's bind
// groups — block faces and the steep ground around them can never
// diverge in texture. Rebuilds when the terrain base republishes.
class CliffSystem {
public:
    static constexpr f32 kMaxWallHeight = 200.0f;
    // Bands farther than this draw their plain-box twin. Tight: at the
    // 35° coverage a mountain region carries ~900 bands and the
    // subdivided near mesh is the frame budget (10 FPS seen at 650 m).
    static constexpr f32 kNearRange = 260.0f;

    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Rebuilds the region meshes when params.base changed (publish).
    void update(rhi::Device& device, const TerrainParams& params);

    // Main opaque pass, right after the terrain (same groups: 0 frame,
    // 1 splat, 2 shadow receivers).
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle splatBindGroup,
              rhi::BindGroupHandle shadowBindGroup, const Vec3& cameraPos,
              const Frustum* frustum = nullptr);

    // Depth-only caster pass (terrain caster shader; the far twin —
    // block shadows do not need the displacement detail).
    void drawDepth(rhi::CommandBuffer& cmd,
                   rhi::BindGroupHandle casterBindGroup,
                   const Frustum* frustum = nullptr);

    u32 wallCount() const { return walls; }
    u32 indicesThisFrame() const { return frameIndices; }
    void beginFrame() { frameIndices = 0; }

private:
    struct BandRange {
        u32 firstIndex { 0 };  // near mesh
        u32 indexCount { 0 };
        u32 farFirstIndex { 0 };
        u32 farIndexCount { 0 };
        Vec3 lo {};
        Vec3 hi {};
    };
    struct RegionMesh {
        rhi::UniqueBuffer vertexBuffer;
        rhi::UniqueBuffer indexBuffer;
        rhi::UniqueBuffer farVertexBuffer;
        rhi::UniqueBuffer farIndexBuffer;
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
