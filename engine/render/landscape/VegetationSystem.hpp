#pragma once

#include <atomic>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include "engine/core/Defines.hpp"
#include "engine/render/Frustum.hpp"
#include "engine/assets/MeshData.hpp"
#include "engine/render/landscape/ChunkStreamer.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/render/landscape/TreeGenerator.hpp" // *TreeParams (builder)
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

// Procedural props: trees, rocks and bushes. A handful of
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
    // The forest scatter's uniform-scale range (hand-tuned).
    static constexpr f32 kTreeScaleMin = 4.8f;
    static constexpr f32 kTreeScaleMax = 8.4f;

    // Measured from the GENERATED tree variant meshes at the mean
    // scatter scale — the far-tree impostors size and shape themselves
    // from it, so retuned trees (and, later, per-type forests) reshape
    // their own distant silhouettes automatically.
    struct TreeSilhouette {
        f32 height { 14.0f };       // mean world height (m)
        f32 widthRatio { 0.9f };    // crown width / height
        f32 trunkFraction { 0.4f }; // bare-trunk share of the height
    };
    TreeSilhouette treeSilhouette() const;
    static constexpr u32 kRockVariants = 4;
    static constexpr u32 kBushVariants = 3;
    static constexpr u32 kVariantCount =
        kTreeVariants + kRockVariants + kBushVariants;
    static constexpr u32 kFirstRock = kTreeVariants;
    static constexpr u32 kFirstBush = kTreeVariants + kRockVariants;
    // Runtime knobs — live-safe: the ring streamer adapts on its own
    // (requestMissing reads the new radius, evictFar drains the excess).
    // NB: the tree FADE tops out at 880 m — radii under ~14 pop at the
    // ring edge instead of fading (a budget-hunting knob, not a look).
    i32 viewRadius { 12 };        // chunks
    i32 highDetailRadius { 2 };   // full-detail canopies within (x 64 m)
    // Third mesh level beyond lowDetailRadius — bare-icosahedron lobes
    // (20 faces, generateTree(seed, 0)): ~150 tris/tree vs ~600 on the
    // low twin. The far ring is where the instances are, so this is
    // where the triangle budget goes (docs/RENDERING.md, V8f).
    i32 lowDetailRadius { 4 };    // 80-face twins within; ultra beyond
    // A/B — tree variants regenerate through generateColonizedTree (Runions
    // skeleton + SDF-normal billboard-card foliage; the default). Flip via
    // reseedVariantMeshes; the lobe trees stay one checkbox away in the
    // Vegetation / Tree builder panels. docs/RENDERING.md brique 27b.
    bool colonizationTrees { true };
    // Tree builder: the generators' knobs, mapped from the
    // *TreeTuningForm records by the scene and edited live by the panel
    // (apply through reseedVariantMeshes). Defaults = shipped look.
    LobeTreeParams lobeTreeParams {};
    ColonizedTreeParams colonizedTreeParams {};
    // Chunk-AABB pads for the culling tests (draw/drawDepth): chunk
    // min/maxY track prop BASES, so Y must absorb the tallest scaled
    // tree (~7.5 m mesh x 11.2 scale) and XZ the widest canopy overhang.
    static constexpr f32 kPropPadXz = 26.0f;
    static constexpr f32 kPropPadY = 86.0f;
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

    // Mesh-only swap (the colonizationTrees A/B): variant meshes rebuild
    // with the current seed, chunks and instance buffers stay resident
    // (instances reference variants by index — nothing to re-scatter).
    // The leaf mask rebuilds too — its knobs ride the same panel.
    void reseedVariantMeshes(rhi::Device& device) {
        destroyVariantMeshes(device);
        createVariantMeshes(device, meshSeed);
        rebuildLeafMask(device);
    }

    // Replaces one variant's mesh with an authored one (glTF
    // rock). The CPU copy is kept so regenerate() re-uploads it after a
    // seed change. uv.x drives canopy sway in tree.vert — zero the uvs for
    // rigid props. Scatter, instancing and shadow casting are untouched.
    void overrideVariantMesh(rhi::Device& device, u32 variant,
                             MeshData mesh);

    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Canopy LOD, three mesh levels from the SAME seed (composition and
    // colors match; only lobe tessellation changes): 320-face lobes within
    // `highDetailRadius`, 80-face twins to `lowDetailRadius`, 20-face
    // ultra beyond. Reflections force ultra (half-res mirror);
    // shadow casters use low near, ultra for the far cascades.

    // `variantLimit` restricts which variants draw (e.g. kTreeVariants for
    // the reflection pass: trees only). Trees are single opaque
    // meshes — no leaf-card overlay pass. `cameraPos` drives the
    // per-chunk LOD pick; `forceLowDetail` = mirrored/downsampled passes
    // (resolves to the ultra level when the variant has one).
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle shadowBindGroup,
              u32 variantLimit = kVariantCount, const Vec3& cameraPos = {},
              bool forceLowDetail = false, const Frustum* frustum = nullptr,
              const std::unordered_set<u64>* occluded = nullptr);

    // Chunks the last culled draw() recorded (for the debug panel).
    u32 drawnLastFrame() const { return lastDrawn; }
    // CPU-side geometry counters, summed across every pass this frame
    // (mid-pass GPU timestamps are meaningless on Metal).
    u32 indicesThisFrame() const { return frameIndices; }
    u32 highDetailInstancesThisFrame() const { return frameHighInstances; }
    u32 lowDetailInstancesThisFrame() const { return frameLowInstances; }
    u32 ultraDetailInstancesThisFrame() const { return frameUltraInstances; }

    // Depth-only caster pass into one shadow cascade (frameBindGroup feeds
    // the sway/fade math, casterBindGroup the cascade's light matrix).
    // Chunks beyond `maxChunkDistance` (Chebyshev) are skipped — cascades
    // only reach so far. `frustum` = the cascade's ortho volume:
    // trees outside it cannot shadow anything in the cascade, and the
    // near cascades cover a fraction of the ring (same rationale as
    // TerrainSystem::drawDepth).
    // `ultraDetail` = far cascades: the 20-face level throws the same
    // soft shadow — cascade 0 keeps the 80-face twin (close-ups).
    void drawDepth(rhi::CommandBuffer& cmd,
                   rhi::BindGroupHandle frameBindGroup,
                   rhi::BindGroupHandle casterBindGroup, const Vec3& cameraPos,
                   i32 maxChunkDistance, const Frustum* frustum = nullptr,
                   bool ultraDetail = false);

    u32 propTotal() const { return instances; }

    // One placed prop. Layout mirrors tree.vert's instance attributes.
    struct Instance {
        Vec4 positionScale; // xyz = terrain point, w = uniform scale
        Vec4 params;        // x = yaw, y = tint jitter, z = sway phase, w free
    };

    // Tool scenes (tree builder): when set, draw()/drawDepth() render
    // ONLY these explicit instances with variant 0's full-detail mesh,
    // and update() skips the scatter streaming — the showcase replaces
    // the streamed world, it does not overlay it. Empty list = off.
    void setShowcase(rhi::Device& device, const vector<Instance>& list);
    bool showcaseActive() const { return showcaseCount != 0; }

    // Async variant reseed: the tree meshes + AO bake run on a worker
    // from a parameter SNAPSHOT (the worker never touches `this` —
    // teardown-safe); the GPU upload lands in update() on the main
    // thread, and the previous meshes keep drawing until the swap.
    // `seed` becomes the mesh seed. A request while one is in flight
    // coalesces into a relaunch with the latest params on landing.
    // reseedProgress() feeds a loading bar (completed generation steps).
    void reseedVariantMeshesAsync(core::JobSystem& jobs, u32 seed);
    bool reseedPending() const { return reseedJob != nullptr; }
    f32 reseedProgress() const;
    // Worker output: instances bucketed per mesh variant.
    using VariantBuckets = array<vector<Instance>, kVariantCount>;

    // GI injection: the vegetation must enter the GI volume (forests
    // bounce green), so a compact CPU copy of each
    // chunk's props survives the GPU upload for the injection to box them.
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
        vector<GiProp> giProps; // CPU copy for the GI injection
    };
    struct VariantMesh {
        rhi::UniqueBuffer vertexBuffer;
        rhi::UniqueBuffer indexBuffer;
        u32 indexCount { 0 };
        // Low-detail twin (tree variants only; empty = use the main mesh).
        rhi::UniqueBuffer lowVertexBuffer;
        rhi::UniqueBuffer lowIndexBuffer;
        u32 lowIndexCount { 0 };
        // Ultra twin (bare-icosahedron lobes; empty = stop at low).
        rhi::UniqueBuffer ultraVertexBuffer;
        rhi::UniqueBuffer ultraIndexBuffer;
        u32 ultraIndexCount { 0 };
        // Shadow proxy for the far cascades (colonized trees: solid
        // metaball blobs instead of the card cloud — see
        // generateColonizedTreeShadowProxy). Empty = cast with the LODs.
        rhi::UniqueBuffer casterVertexBuffer;
        rhi::UniqueBuffer casterIndexBuffer;
        u32 casterIndexCount { 0 };
    };

    void createVariantMeshes(rhi::Device& device, u32 terrainSeed);
    void destroyVariantMeshes(rhi::Device& device);
    void uploadVariantMesh(rhi::Device& device, u32 variant,
                           const MeshData& mesh);
    void uploadLowDetailMesh(rhi::Device& device, u32 variant,
                             const MeshData& mesh);
    void uploadUltraDetailMesh(rhi::Device& device, u32 variant,
                               const MeshData& mesh);
    void uploadShadowProxyMesh(rhi::Device& device, u32 variant,
                               const MeshData& mesh);
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void buildCasterPipeline(rhi::Device& device, ShaderLibrary& shaders);
    // (Re)generates the shared leaf-cluster cutout mask the foliage cards
    // sample (generateLeafMaskPixels) and its bind group.
    void rebuildLeafMask(rhi::Device& device);

    // The shared ring mechanics live in ChunkStreamer.
    ChunkStreamer<Chunk, VariantBuckets> streamer;
    u32 meshSeed { 0 }; // last create/regenerate seed (reseedVariantMeshes)
    u32 instances { 0 };
    u32 lastDrawn { 0 };
    u32 frameIndices { 0 };       // reset in update(), summed by draw*()
    u32 frameHighInstances { 0 };  // 320-face canopies drawn this frame
    u32 frameLowInstances { 0 };   // low-twin instances drawn this frame
    u32 frameUltraInstances { 0 }; // ultra-twin instances drawn this frame

    array<VariantMesh, kVariantCount> variantMeshes {};
    // Per tree variant, mesh units: x = height, y = max radial extent,
    // z = crown start height. Zero until the variant lands.
    array<Vec3, kTreeVariants> treeBounds {};
    std::unordered_map<u32, MeshData> meshOverrides;
    // Shared leaf-cluster cutout mask (all tree variants; cards only).
    rhi::UniqueTexture leafMask;
    rhi::UniqueSampler leafMaskSampler;
    rhi::UniqueBindGroup leafMaskGroup;
    rhi::UniquePipeline pipeline;
    u64 shaderGeneration { 0 };
    rhi::UniquePipeline casterPipeline;
    u64 casterShaderGeneration { 0 };
    // Showcase mode (tool scenes): explicit instances of variant 0.
    rhi::UniqueBuffer showcaseInstances;
    u32 showcaseCount { 0 };

    // Async reseed state (reseedVariantMeshesAsync): inputs snapshotted
    // by value, outputs filled by the worker, swapped in pumpReseed().
    struct ReseedJob {
        u32 seed { 0 };
        bool colonization { true };
        LobeTreeParams lobes;
        ColonizedTreeParams colonized;
        std::filesystem::path aoCacheDir;
        array<array<MeshData, 3>, kTreeVariants> lods; // [variant][lod]
        array<MeshData, kTreeVariants> casters;        // colonization only
        std::atomic<u32> completed { 0 };
        u32 total { 1 };
        std::atomic<bool> done { false };
    };
    void pumpReseed(rhi::Device& device); // main thread, from update()
    sptr<ReseedJob> reseedJob;
    core::JobSystem* reseedJobs { nullptr };
    bool reseedQueued { false };
    u32 reseedQueuedSeed { 0 };
};

// Pure CPU scatter for one chunk, runs on worker threads. Deterministic.
// The forest-belt mask (broad noise thresholded) — FarTerrain raises and
// darkens its coarse mesh with the SAME mask, so the distant forest
// fringe continues the real scatter past the vegetation ring.
f32 forestMask(u32 seed, f32 x, f32 z);

VegetationSystem::VariantBuckets scatterProps(const TerrainParams& params,
                                              i32 cx, i32 cz);

} // namespace render
