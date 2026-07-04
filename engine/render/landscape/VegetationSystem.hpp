#pragma once

#include <unordered_map>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/Frustum.hpp"
#include "engine/render/MeshData.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"

namespace core {
class JobSystem;
}
namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Procedural props: trees, rocks and bushes (bricks 15+16). A handful of
// generated mesh variants are built once; worker threads scatter instances
// per chunk — trees into FOREST BELTS (low-frequency mask over grassy,
// gently sloped, mid-altitude ground), rocks sparsely everywhere including
// the alpine zone, bushes on grass with a bias toward forest edges — all
// deterministic per seed. Streaming skeleton identical to GrassSystem, with
// a wider ring since props carry to the fog line. Draws are grouped
// variant-major: one instanced draw per (variant, chunk) pair, using
// firstInstance offsets into the chunk's variant-sorted instance buffer.
class VegetationSystem {
public:
    static constexpr u32 kTreeVariants = 5;
    static constexpr u32 kRockVariants = 4;
    static constexpr u32 kBushVariants = 3;
    static constexpr u32 kVariantCount =
        kTreeVariants + kRockVariants + kBushVariants;
    static constexpr u32 kFirstRock = kTreeVariants;
    static constexpr u32 kFirstBush = kTreeVariants + kRockVariants;
    static constexpr i32 kViewRadius = 14; // chunks (~900 m; fade at 880 m)
    static constexpr i32 kEvictRadius = 15;
    static constexpr u32 kMaxUploadsPerFrame = 2;

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobSystem, u32 terrainSeed);
    void destroy(rhi::Device& device);

    // Streaming pump — main thread, once per frame (top of render).
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& cameraPos);

    // Drops every chunk (terrain seed changed). Variant meshes are reseeded
    // on the next update.
    void regenerate(rhi::Device& device, u32 terrainSeed);

    // Replaces one variant's mesh with an authored one (brick 23: glTF
    // rock). The CPU copy is kept so regenerate() re-uploads it after a
    // seed change. uv.x drives canopy sway in tree.vert — zero the uvs for
    // rigid props. Scatter, instancing and shadow casting are untouched.
    void overrideVariantMesh(rhi::Device& device, u32 variant,
                             MeshData mesh);

    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Leaf cards cut out at ~300 m (leaf.vert fade); chunks beyond this
    // Chebyshev radius skip the leaf pass entirely (vertex work, not just
    // fill).
    static constexpr i32 kLeafChunkRadius = 5; // × 64 m = 320 m

    // `variantLimit` restricts which variants draw (e.g. kTreeVariants for
    // the reflection pass: trees only). `withLeaves` adds the alpha-cutout
    // leaf-card pass over the tree variants, for chunks near `cameraPos`
    // only — skipped in the reflection (the solid blobs carry the mirrored
    // silhouette for far less fill).
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle shadowBindGroup,
              u32 variantLimit = kVariantCount, bool withLeaves = true,
              const Vec3& cameraPos = {}, const Frustum* frustum = nullptr);

    // Chunks the last culled draw() recorded (for the debug panel).
    u32 drawnLastFrame() const { return lastDrawn; }

    // Depth-only caster pass into one shadow cascade (frameBindGroup feeds
    // the sway/fade math, casterBindGroup the cascade's light matrix).
    // Chunks beyond `maxChunkDistance` (Chebyshev) are skipped — cascades
    // only reach so far.
    void drawDepth(rhi::CommandBuffer& cmd,
                   rhi::BindGroupHandle frameBindGroup,
                   rhi::BindGroupHandle casterBindGroup, const Vec3& cameraPos,
                   i32 maxChunkDistance);

    u32 propTotal() const { return instances; }

    // One placed prop. Layout mirrors tree.vert's instance attributes.
    struct Instance {
        Vec4 positionScale; // xyz = terrain point, w = uniform scale
        Vec4 params;        // x = yaw, y = tint jitter, z = sway phase, w free
    };
    // Worker output: instances bucketed per mesh variant.
    using VariantBuckets = array<vector<Instance>, kVariantCount>;

private:
    struct Chunk {
        bool resident { false };
        rhi::BufferHandle instanceBuffer {};
        array<u32, kVariantCount> counts {};
        array<u32, kVariantCount> firstInstance {};
        u32 total { 0 };
        // Prop-base height range, for the frustum AABB.
        f32 minY { 0.0f };
        f32 maxY { 0.0f };
    };
    struct BuiltChunk {
        i32 cx { 0 };
        i32 cz { 0 };
        u64 generation { 0 };
        VariantBuckets buckets;
    };
    struct Shared {
        core::ConcurrentQueue<BuiltChunk> built;
    };
    struct VariantMesh {
        rhi::BufferHandle vertexBuffer {};
        rhi::BufferHandle indexBuffer {};
        u32 indexCount { 0 };
        // Leaf-card overlay (tree variants only; empty elsewhere).
        rhi::BufferHandle leafVertexBuffer {};
        rhi::BufferHandle leafIndexBuffer {};
        u32 leafIndexCount { 0 };
    };

    static u64 keyOf(i32 cx, i32 cz) {
        return (static_cast<u64>(static_cast<u32>(cx)) << 32) |
               static_cast<u32>(cz);
    }

    void createVariantMeshes(rhi::Device& device, u32 terrainSeed);
    void destroyVariantMeshes(rhi::Device& device);
    void uploadVariantMesh(rhi::Device& device, u32 variant,
                           const MeshData& mesh);
    void uploadLeafMesh(rhi::Device& device, u32 variant,
                        const MeshData& mesh);
    void buildLeafPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void buildCasterPipeline(rhi::Device& device, ShaderLibrary& shaders);

    sptr<Shared> shared;
    core::JobSystem* jobs { nullptr };

    std::unordered_map<u64, Chunk> chunks;
    u64 generation { 0 };
    u32 instances { 0 };
    u32 lastDrawn { 0 };

    array<VariantMesh, kVariantCount> variantMeshes {};
    std::unordered_map<u32, MeshData> meshOverrides;
    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
    rhi::PipelineHandle casterPipeline {};
    u64 casterShaderGeneration { 0 };

    // Leaf-card pass: bouquet atlas + its own alpha-cutout pipeline.
    rhi::TextureHandle leafTexture {};
    rhi::SamplerHandle leafSampler {};
    rhi::BindGroupHandle leafBindGroup {};
    rhi::PipelineHandle leafPipeline {};
    u64 leafShaderGeneration { 0 };
};

// Pure CPU scatter for one chunk, runs on worker threads. Deterministic.
VegetationSystem::VariantBuckets scatterProps(const TerrainParams& params,
                                              i32 cx, i32 cz);

} // namespace render
