#pragma once

#include <unordered_map>
#include <unordered_set>

#include "engine/core/Defines.hpp"
#include "engine/render/Frustum.hpp"
#include "engine/assets/MeshData.hpp"
#include "engine/render/landscape/ChunkStreamer.hpp"
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
    // GPU-PERF P1: runtime knobs (were compile-time — mainVeg measured
    // 1.8 ms at 14). Live-safe: the ring streamer adapts on its own
    // (requestMissing reads the new radius, evictFar drains the excess).
    // NB: the tree FADE tops out at 880 m — radii under ~14 pop at the
    // ring edge instead of fading (a budget-hunting knob, not a look).
    i32 viewRadius { 12 };        // chunks (dev pick 2026-07-10)
    i32 highDetailRadius { 5 };   // 320-face canopies within (x 64 m)
    static constexpr u32 kMaxUploadsPerFrame = 2;
    // Scatter jobs budgeted like uploads (see TerrainSystem — the
    // unbudgeted ring edge was part of the fast-travel stutter).
    static constexpr u32 kMaxRequestsPerFrame = 4;

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobSystem, u32 terrainSeed);
    void destroy(rhi::Device& device);

    // Streaming pump — main thread, once per frame (top of render).
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& cameraPos);

    // Drops every chunk (terrain seed changed). Variant meshes are reseeded
    // on the next update.
    void regenerate(rhi::Device& device, u32 terrainSeed);

    // Drops only these chunks (cx,cz keys) so update() re-scatters props onto
    // the current terrain — the sculpt path. Non-resident chunks are left to
    // finish streaming. Keys share the terrain chunk grid (keyOf).
    void invalidateChunks(rhi::Device& device, const vector<u64>& keys);

    // Replaces one variant's mesh with an authored one (brick 23: glTF
    // rock). The CPU copy is kept so regenerate() re-uploads it after a
    // seed change. uv.x drives canopy sway in tree.vert — zero the uvs for
    // rigid props. Scatter, instancing and shadow casting are untouched.
    void overrideVariantMesh(rhi::Device& device, u32 variant,
                             MeshData mesh);

    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Canopy LOD: chunks within `highDetailRadius` (above) draw the
    // 320-face lobes; everything beyond gets the 80-face LOD (same seed,
    // same silhouette — 4x fewer vertices where facets are invisible
    // anyway). Shadow casters and reflections always use the low LOD.

    // `variantLimit` restricts which variants draw (e.g. kTreeVariants for
    // the reflection pass: trees only). Brick 27: trees are single opaque
    // meshes — no leaf-card overlay pass anymore. `cameraPos` drives the
    // per-chunk LOD pick; `forceLowDetail` = mirrored/downsampled passes.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle shadowBindGroup,
              u32 variantLimit = kVariantCount, const Vec3& cameraPos = {},
              bool forceLowDetail = false, const Frustum* frustum = nullptr,
              const std::unordered_set<u64>* occluded = nullptr);

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

    // Chantier RC (dev report 2026-07-11: forests bounced no green — the
    // vegetation never entered the GI volume): a compact CPU copy of each
    // chunk's props survives the GPU upload so the injection can box them.
    struct GiProp {
        Vec3 position; // terrain point (prop base)
        f32 scale;     // uniform scale
        u8 kind;       // 0 = tree, 1 = rock, 2 = bush
    };
    // Appends props within `halfSpan` (Chebyshev) of `center`, nearest
    // chunks first, until `out` reaches `maxProps`.
    void collectGiProps(const Vec3& center, f32 halfSpan,
                        vector<GiProp>& out, size_t maxProps) const;

private:
    struct Chunk {
        bool resident { false };
        rhi::UniqueBuffer instanceBuffer;
        array<u32, kVariantCount> counts {};
        array<u32, kVariantCount> firstInstance {};
        u32 total { 0 };
        // Prop-base height range, for the frustum AABB.
        f32 minY { 0.0f };
        f32 maxY { 0.0f };
        vector<GiProp> giProps; // chantier RC: CPU copy for the injection
    };
    struct VariantMesh {
        rhi::UniqueBuffer vertexBuffer;
        rhi::UniqueBuffer indexBuffer;
        u32 indexCount { 0 };
        // Low-detail twin (tree variants only; empty = use the main mesh).
        rhi::UniqueBuffer lowVertexBuffer;
        rhi::UniqueBuffer lowIndexBuffer;
        u32 lowIndexCount { 0 };
    };

    void createVariantMeshes(rhi::Device& device, u32 terrainSeed);
    void destroyVariantMeshes(rhi::Device& device);
    void uploadVariantMesh(rhi::Device& device, u32 variant,
                           const MeshData& mesh);
    void uploadLowDetailMesh(rhi::Device& device, u32 variant,
                             const MeshData& mesh);
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void buildCasterPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // The shared ring mechanics (audit U3-1) live in ChunkStreamer.
    ChunkStreamer<Chunk, VariantBuckets> streamer;
    u32 instances { 0 };
    u32 lastDrawn { 0 };

    array<VariantMesh, kVariantCount> variantMeshes {};
    std::unordered_map<u32, MeshData> meshOverrides;
    rhi::UniquePipeline pipeline;
    u64 shaderGeneration { 0 };
    rhi::UniquePipeline casterPipeline;
    u64 casterShaderGeneration { 0 };
};

// Pure CPU scatter for one chunk, runs on worker threads. Deterministic.
VegetationSystem::VariantBuckets scatterProps(const TerrainParams& params,
                                              i32 cx, i32 cz);

} // namespace render
