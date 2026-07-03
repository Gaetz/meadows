#pragma once

#include <unordered_map>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
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

// Streaming heightmap terrain (bricks 7+8). A ring of chunks follows the
// camera: missing chunks are meshed on JobSystem workers (pure TerrainNoise
// sampling), results come back through a non-blocking ConcurrentQueue, and
// the main thread uploads a budgeted few per frame — the frame never blocks
// on terrain, a not-yet-resident chunk simply isn't drawn (holes stay at the
// horizon because requests go out center-first). Chunks beyond an eviction
// radius (hysteresis over the view radius) free their GPU memory; revisited
// terrain is bit-identical because meshing is deterministic. Same
// worker->queue->pump pattern as game/TextureCache (Phase 5).
//
// LOD (brick 8): discrete per-chunk meshes (65²/33²/17²/9² vertices) selected
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
    static constexpr i32 kViewRadius = 14;    // chunks (Chebyshev), ~900 m
    static constexpr i32 kEvictRadius = 16;   // > view radius: hysteresis
    static constexpr u32 kMaxUploadsPerFrame = 8;

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

    // Rebuilds the pipeline when the terrain shader hot-reloaded.
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Debug wireframe (rebuilds the pipeline when the flag changes).
    void setWireframe(bool enabled, rhi::Device& device,
                      ShaderLibrary& shaders);
    bool isWireframe() const { return wireframe; }

    // Records terrain draws into the current render pass.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup);

    // Streaming stats for the debug panel.
    u32 residentCount() const { return resident; }
    u32 pendingCount() const { return pending; }
    u32 uploadsLastFrame() const { return lastUploads; }

    TerrainParams params {};

private:
    static constexpr u8 kNoLod = 0xff;

    struct Chunk {
        // Drawn mesh; kNoLod until the first upload lands.
        u8 residentLod { kNoLod };
        // LOD requested from a worker; kNoLod when nothing is in flight
        // (guards to one in-flight job per chunk).
        u8 queuedLod { kNoLod };
        rhi::BufferHandle vertexBuffer {};
    };
    // A worker's finished mesh. `generation` stamps which world it belongs
    // to; stale results (after regenerate) are dropped on arrival.
    struct BuiltChunk {
        i32 cx { 0 };
        i32 cz { 0 };
        u8 lod { 0 };
        u64 generation { 0 };
        vector<MeshVertex> vertices;
    };
    // Owned via shared_ptr and captured by every worker job, so a job that
    // outlives this system still has a valid queue to push into (the
    // TextureCache teardown-safety pattern).
    struct Shared {
        core::ConcurrentQueue<BuiltChunk> built;
    };

    static u64 keyOf(i32 cx, i32 cz) {
        return (static_cast<u64>(static_cast<u32>(cx)) << 32) |
               static_cast<u32>(cz);
    }

    void pumpUploads(rhi::Device& device);
    void requestMissing(const Vec3& cameraPos);
    void enqueueBuild(i32 cx, i32 cz, u8 lod);
    void evictFar(rhi::Device& device, const Vec3& cameraPos);
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    sptr<Shared> shared;
    core::JobSystem* jobs { nullptr };

    std::unordered_map<u64, Chunk> chunks;
    u64 generation { 0 };
    u32 resident { 0 };
    u32 pending { 0 };
    u32 lastUploads { 0 };
    bool wireframe { false };

    // Chunks of equal LOD share one index buffer (identical topology).
    array<rhi::BufferHandle, kLodCount> indexBuffers {};
    array<u32, kLodCount> indexCounts {};
    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
};

// Pure CPU chunk meshing, runs on worker threads. Vertices sample
// height/normal/color in world space at grid coord (cx, cz); the grid ring is
// followed by the skirt ring (edge vertices extruded down). The index
// topology depends only on the LOD.
vector<MeshVertex> buildChunkVertices(const TerrainParams& params, i32 cx,
                                      i32 cz, u32 lod);
vector<u32> buildChunkIndices(u32 lod);

} // namespace render
