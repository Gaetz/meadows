#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/MeshData.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Heightmap terrain renderer. Brick 6 scope: a static 9x9 chunk grid built
// synchronously at creation, uniform LOD — worker streaming and LOD rings
// arrive in the next bricks. Heights are sampled in world space from
// TerrainNoise, so adjacent chunks match at their borders by construction.
class TerrainSystem {
public:
    static constexpr f32 kChunkSize = 64.0f; // meters
    static constexpr u32 kChunkQuads = 64;   // LOD0: 65x65 vertices
    static constexpr i32 kGridHalfExtent = 4; // 9x9 chunks around the origin

    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);

    // Rebuilds the pipeline when the terrain shader hot-reloaded.
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Records terrain draws into the current render pass.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup);

    u32 chunkCount() const { return static_cast<u32>(chunks.size()); }

    TerrainParams params {};

private:
    struct Chunk {
        i32 cx { 0 };
        i32 cz { 0 };
        rhi::BufferHandle vertexBuffer {};
    };

    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    vector<Chunk> chunks;
    // All chunks share one index buffer: identical grid topology at equal LOD.
    rhi::BufferHandle indexBuffer {};
    u32 indexCount { 0 };
    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
};

// Pure CPU chunk meshing (worker-thread material for the streaming brick).
// Vertices sample height/normal/color in world space at grid coord (cx, cz);
// the index topology is chunk-independent.
vector<MeshVertex> buildChunkVertices(const TerrainParams& params, i32 cx,
                                      i32 cz);
vector<u32> buildChunkIndices();

} // namespace render
