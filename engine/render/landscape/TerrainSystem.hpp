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

// Heightmap terrain renderer. Brick 5 scope: ONE chunk built synchronously at
// creation — the chunk map, LODs, and worker streaming arrive in later bricks.
// Heights are sampled in world space from TerrainNoise, so future adjacent
// chunks match at their borders by construction.
class TerrainSystem {
public:
    static constexpr f32 kChunkSize = 64.0f; // meters
    static constexpr u32 kChunkQuads = 64;   // LOD0: 65x65 vertices

    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);

    // Rebuilds the pipeline when the terrain shader hot-reloaded.
    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Records terrain draws into the current render pass.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup);

    TerrainParams params {};

private:
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    rhi::BufferHandle vertexBuffer {};
    rhi::BufferHandle indexBuffer {};
    u32 indexCount { 0 };
    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
};

// Builds the render mesh of one chunk at grid coordinate (cx, cz), sampling
// heights/normals/colors in world space. Pure CPU work — this is the function
// the streaming brick moves onto worker threads.
MeshData buildChunkMesh(const TerrainParams& params, i32 cx, i32 cz);

} // namespace render
