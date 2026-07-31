#pragma once

// Subsystem map: docs/AUDIT/U3-renderer-landscape.md

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

// Streaming heightmap terrain (bricks 7+8). A ring of chunks follows the
// camera: missing chunks are meshed on JobSystem workers (pure TerrainNoise
// sampling), results come back through a non-blocking ConcurrentQueue, and
// the main thread uploads a budgeted few per frame — the frame never blocks
// on terrain, a not-yet-resident chunk simply isn't drawn (holes stay at the
// horizon because requests go out center-first). Chunks beyond an eviction
// radius (hysteresis over the view radius) free their GPU memory; revisited
// terrain is bit-identical because meshing is deterministic. Same
// worker->queue->pump pattern as engine/render/TextureCache (Phase 5).
//
// LOD: discrete per-chunk meshes (65²/33²/17²/9² vertices) selected
// by camera distance. Every mesh carries a SKIRT — its edge ring extruded
// down — so T-junction cracks between neighboring LODs are covered with zero
// stitching logic; a chunk at any LOD is self-contained (what the
// job-per-chunk model wants). On LOD change the old mesh keeps drawing until
// the new one is resident, then swaps (no holes, no popping to nothing).
class TerrainSystem {
public:
    static constexpr f32 kChunkSize = 64.0f;  // meters
    static constexpr u32 kChunkQuads = 64;    // LOD0: 65x65 vertices
    static constexpr u32 kLodCount = 4;       // 64/32/16/8 quads per side
    // Streaming ring radius in chunks (Chebyshev) — the draw distance,
    // live-tunable (LandscapeTuningForm::terrainViewRadius; applyFog's
    // horizon closure tracks it). Evict = +2 chunks of hysteresis.
    i32 viewRadius { 15 };
    static constexpr u32 kMaxUploadsPerFrame = 8;
    // Time cap on top of the count cap (LOD0 uploads dwarf LOD3 ones).
    static constexpr f64 kUploadMsBudget = 2.0;
    // Requests are budgeted too (nearest first): a border crossing used to
    // dump the whole leading ring edge + the LOD-swap wave on the workers
    // at once — every core saturates and the frame thread starves until
    // `pending` drains (the fast-travel stutter). Matching the upload
    // budget keeps the steady-state fill rate; it only flattens the burst.
    static constexpr u32 kMaxRequestsPerFrame = 8;

    static constexpr u32 lodQuads(u32 lod) { return kChunkQuads >> lod; }
    // Ring distances: LOD0 under the camera, then 1/3/6 chunk rings.
    static constexpr u32 lodForDistance(i32 chebyshev) {
        if (chebyshev <= 1) { return 0; }
        if (chebyshev <= 3) { return 1; }
        if (chebyshev <= 6) { return 2; }
        return 3;
    }

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobSystem);
    void destroy(rhi::Device& device);

    // Streaming pump — main thread, once per frame, top of render: drains
    // finished meshes (budgeted uploads), requests missing chunks around the
    // camera, evicts far ones.
    void update(rhi::Device& device, const Vec3& cameraPos);

    // Drops every chunk and re-streams with current params (seed changed).
    // In-flight worker results are invalidated by generation.
    void regenerate(rhi::Device& device);

    // Re-mesh only these chunks (cx,cz keys) in place — the terrain-sculpt
    // path. Each keeps drawing its current mesh until the rebuilt one (with the
    // new params) is resident, then swaps (no hole). Non-resident or
    // already-building chunks are skipped; they pick up the new params when
    // they next stream. Keys use keyOf() — the shared terrain chunk grid.
    void remeshChunks(const vector<u64>& keys);

    // Rebuilds the pipeline when the terrain shader hot-reloaded.
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Debug wireframe (rebuilds the pipeline when the flag changes).
    void setWireframe(bool enabled, rhi::Device& device,
                      ShaderLibrary& shaders);
    bool isWireframe() const { return wireframe; }

    // Records terrain draws into the current render pass. `shadowBindGroup`
    // provides the CSM map + comparison sampler (texture unit 1). When a
    // frustum is given, chunks whose AABB (XZ footprint × meshed [minY,
    // maxY]) lies outside are skipped; `occluded` additionally
    // drops chunks hidden behind terrain (main view only —
    // the set is built for the real camera, not the mirrored one).
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle shadowBindGroup,
              const Frustum* frustum = nullptr,
              const std::unordered_set<u64>* occluded = nullptr);

    // GPU-driven main-pass path (docs/RENDERING.md §6.0): per LOD, ONE
    // drawIndexedIndirect over the command range the chunk_cull dispatch
    // wrote LAST frame (culled chunks carry instanceCount 0). The caller
    // checked commandsValid() on the provider.
    void drawIndirect(rhi::CommandBuffer& cmd,
                      rhi::BindGroupHandle frameBindGroup,
                      rhi::BindGroupHandle shadowBindGroup,
                      rhi::BufferHandle commands, const u32* lodFirst,
                      const u32* lodCount); // kLodCount entries each

    // Resident chunk AABBs, the GPU occlusion candidate list — carries the
    // indirect-draw parameters (group = lod, pool slot as vertexOffset).
    struct ChunkAabb {
        u64 key { 0 };
        Vec3 lo {};
        Vec3 hi {};
        u32 group { 0 };       // lod — indirect commands batch per group
        u32 indexCount { 0 };
        i32 vertexOffset { 0 };
    };
    void collectChunkAabbs(vector<ChunkAabb>& out) const {
        out.clear();
        out.reserve(streamer.chunks.size());
        for (const auto& [key, chunk] : streamer.chunks) {
            if (chunk.residentLod == kNoLod) {
                continue;
            }
            const f32 x0 = static_cast<f32>(chunkKeyCx(key)) * kChunkSize;
            const f32 z0 = static_cast<f32>(chunkKeyCz(key)) * kChunkSize;
            const u32 lod = chunk.residentLod;
            out.push_back({ key,
                            { x0, chunk.minY, z0 },
                            { x0 + kChunkSize, chunk.maxY, z0 + kChunkSize },
                            lod, indexCounts[lod],
                            static_cast<i32>(chunk.poolSlot *
                                             pools[lod].slotVerts) });
        }
    }

    // Meshed maxY per resident chunk — the occlusion rebuild's target
    // table (copied only when a rebuild starts, ~1/s).
    std::unordered_map<u64, f32> chunkTops() const {
        std::unordered_map<u64, f32> tops;
        tops.reserve(streamer.chunks.size());
        for (const auto& [key, chunk] : streamer.chunks) {
            if (chunk.residentLod != kNoLod) {
                tops.emplace(key, chunk.maxY);
            }
        }
        return tops;
    }

    // Depth-only caster pass into one shadow cascade. `casterBindGroup`
    // carries the cascade's light matrix; chunks beyond `maxChunkDistance`
    // (Chebyshev, from the camera chunk) are skipped. `frustum` = the
    // cascade's ortho volume: a caster outside it cannot write a
    // useful texel — without this test every cascade paid the full ring's
    // vertex work (cascade 0 covers ~45 m but drew 576 m of chunks; the
    // CSM cost is vertex-bound, which is why the resolution knob did
    // not move it).
    void drawDepth(rhi::CommandBuffer& cmd,
                   rhi::BindGroupHandle casterBindGroup, const Vec3& cameraPos,
                   i32 maxChunkDistance, const Frustum* frustum = nullptr);

    // Streaming stats for the debug panel.
    u32 residentCount() const { return resident; }
    u32 pendingCount() const { return pending; }
    u32 uploadsLastFrame() const { return lastUploads; }
    // Chunks the last culled draw() actually recorded (main pass runs last).
    u32 drawnLastFrame() const { return lastDrawn; }
    // Indices recorded this frame across EVERY pass (casters, reflection,
    // main) — the CPU-side geometry counter (mid-pass GPU timestamps
    // are meaningless on Metal, this is how the vertex load is dissected).
    u32 indicesThisFrame() const { return frameIndices; }

    TerrainParams params {};

private:
    static constexpr u8 kNoLod = 0xff;
    static constexpr u32 kNoSlot = 0xffffffffu;

    struct Chunk {
        // Drawn mesh; kNoLod until the first upload lands.
        u8 residentLod { kNoLod };
        // LOD requested from a worker; kNoLod when nothing is in flight
        // (guards to one in-flight job per chunk).
        u8 queuedLod { kNoLod };
        // Fixed-size slot in the resident LOD's vertex pool. The slot
        // index doubles as the indirect draw's vertexOffset
        // (slot × slotVerts) — the reason chunks share pooled storage.
        u32 poolSlot { kNoSlot };
        // Meshed height range (skirts included), for the frustum AABB.
        f32 minY { 0.0f };
        f32 maxY { 0.0f };
    };

    // One pooled vertex buffer per LOD: every chunk of that LOD is a
    // fixed-size slot (vertex counts are deterministic per LOD). Sized
    // for the tuning slider's MAX view radius — no growth path needed
    // (~30 MB total; a full-slot pool drops the upload with a warning
    // and the chunk re-streams next frame).
    struct VertexPool {
        rhi::UniqueBuffer buffer;
        u32 slotVerts { 0 };
        u32 capacity { 0 };
        vector<u32> freeSlots; // LIFO
        // Freed slots cool for TWO frames before reuse: an indirect
        // command still referencing the old vertexOffset can be consumed
        // one frame late (ping-pong) plus one more under cull
        // back-pressure — reusing the slot inside that window could draw
        // another chunk's mesh there. (Overwrite-in-place of a LIVE slot
        // is safe by construction: the upload queue waits last frame's
        // graphics before any copy.)
        array<vector<u32>, 2> cooling;
    };
    // A worker's finished mesh (the streamer stamps cx/cz/generation).
    struct BuiltMesh {
        u8 lod { 0 };
        vector<MeshVertex> vertices;
        f32 minY { 0.0f };
        f32 maxY { 0.0f };
    };

    void requestMissing(const Vec3& cameraPos);
    void enqueueBuild(i32 cx, i32 cz, u8 lod);
    void evictFar(rhi::Device& device, const Vec3& cameraPos);
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);
    void buildCasterPipeline(rhi::Device& device, ShaderLibrary& shaders);

    void pumpUploads(rhi::Device& device, const Vec3& cameraPos);
    u32 allocSlot(u32 lod);
    void freeSlot(u32 lod, u32 slot);
    // Pool-full relief: free the slot of the FURTHEST chunk resident at
    // this LOD (it is overdue for a LOD swap anyway). Without it, fast
    // flight livelocks: stale far chunks hold every near-LOD slot while
    // the center-out request budget is consumed by the near ring's
    // doomed re-requests — the swaps that would free the slots are never
    // even asked for.
    bool stealFurthestSlot(u32 lod, const Vec3& cameraPos);

    // The shared ring mechanics (audit U3-1): map + generation-stamped
    // queue + budgeted request/evict live in ChunkStreamer.
    ChunkStreamer<Chunk, BuiltMesh> streamer;
    array<VertexPool, kLodCount> pools;
    u32 resident { 0 };
    u32 pending { 0 };
    u32 lastUploads { 0 };
    u32 lastDrawn { 0 };
    u32 frameIndices { 0 }; // reset in update(), summed by draw*()
    bool wireframe { false };

    // Chunks of equal LOD share one index buffer (identical topology).
    array<rhi::UniqueBuffer, kLodCount> indexBuffers;
    array<u32, kLodCount> indexCounts {};
    // Grid triangles only (skirts excluded): the shadow pass uses this —
    // skirts are vertical walls along chunk borders and would cast shadow
    // lines onto neighboring terrain.
    array<u32, kLodCount> gridIndexCounts {};
    rhi::UniquePipeline pipeline;
    u64 shaderGeneration { 0 };
    rhi::UniquePipeline casterPipeline;
    u64 casterShaderGeneration { 0 };

    // Splat material array (grass/rock/snow/sand tiles) + anisotropic
    // repeat sampler, bound as bind group 1 by draw().
    rhi::UniqueTexture splatTexture;
    rhi::UniqueSampler splatSampler;
    rhi::UniqueBindGroup splatBindGroup;
};

// Pure CPU chunk meshing, runs on worker threads. Vertices sample
// height/normal/color in world space at grid coord (cx, cz); the grid ring is
// followed by the skirt ring (edge vertices extruded down). The index
// topology depends only on the LOD.
vector<MeshVertex> buildChunkVertices(const TerrainParams& params, i32 cx,
                                      i32 cz, u32 lod);
vector<u32> buildChunkIndices(u32 lod);

// The shared vertex-tint palette (sand/grass/rock/snow) — FarTerrain
// paints with it too, so the streaming-ring hand-off matches.
Vec3 terrainColor(f32 height, const Vec3& normal, f32 seaLevel);

} // namespace render
