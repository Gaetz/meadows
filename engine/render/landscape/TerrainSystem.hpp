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

// Streaming heightmap terrain (brick 7). A ring of chunks follows the camera:
// missing chunks are meshed on JobSystem workers (pure TerrainNoise sampling),
// results come back through a non-blocking ConcurrentQueue, and the main
// thread uploads a budgeted few per frame — the frame never blocks on
// terrain, a not-yet-resident chunk simply isn't drawn (holes stay at the
// horizon because requests go out center-first). Chunks beyond an eviction
// radius (hysteresis over the view radius) free their GPU memory; revisited
// terrain is bit-identical because meshing is deterministic. Same
// worker->queue->pump pattern as game/TextureCache (Phase 5).
class TerrainSystem {
public:
    static constexpr f32 kChunkSize = 64.0f;  // meters
    static constexpr u32 kChunkQuads = 64;    // LOD0: 65x65 vertices
    static constexpr i32 kViewRadius = 6;     // chunks (Chebyshev)
    static constexpr i32 kEvictRadius = 8;    // > view radius: hysteresis
    static constexpr u32 kMaxUploadsPerFrame = 4;

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

    // Records terrain draws into the current render pass.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup);

    // Streaming stats for the debug panel.
    u32 residentCount() const { return resident; }
    u32 pendingCount() const { return pending; }
    u32 uploadsLastFrame() const { return lastUploads; }

    TerrainParams params {};

private:
    enum class ChunkState { Queued, Resident };
    struct Chunk {
        ChunkState state { ChunkState::Queued };
        rhi::BufferHandle vertexBuffer {};
    };
    // A worker's finished mesh. `generation` stamps which world it belongs
    // to; stale results (after regenerate) are dropped on arrival.
    struct BuiltChunk {
        i32 cx { 0 };
        i32 cz { 0 };
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
    void evictFar(rhi::Device& device, const Vec3& cameraPos);
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    sptr<Shared> shared;
    core::JobSystem* jobs { nullptr };

    std::unordered_map<u64, Chunk> chunks;
    u64 generation { 0 };
    u32 resident { 0 };
    u32 pending { 0 };
    u32 lastUploads { 0 };

    // All chunks share one index buffer: identical grid topology at equal LOD.
    rhi::BufferHandle indexBuffer {};
    u32 indexCount { 0 };
    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
};

// Pure CPU chunk meshing, runs on worker threads. Vertices sample
// height/normal/color in world space at grid coord (cx, cz); the index
// topology is chunk-independent.
vector<MeshVertex> buildChunkVertices(const TerrainParams& params, i32 cx,
                                      i32 cz);
vector<u32> buildChunkIndices();

} // namespace render
