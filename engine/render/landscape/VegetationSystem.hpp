#pragma once

#include <unordered_map>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
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

// Procedural trees (brick 15; rocks and bushes join in brick 16). A handful
// of TreeGenerator mesh variants are built once; worker threads scatter
// instances per chunk into FOREST BELTS (low-frequency mask over grassy,
// gently sloped, mid-altitude ground), deterministic per seed. Streaming
// skeleton identical to GrassSystem, with a wider ring since trees carry to
// the fog line. Draws are grouped variant-major: one instanced draw per
// (variant, chunk) pair, using firstInstance offsets into the chunk's
// variant-sorted instance buffer.
class VegetationSystem {
public:
    static constexpr u32 kTreeVariants = 5;
    static constexpr i32 kViewRadius = 7; // chunks (~480 m; fade covers edge)
    static constexpr i32 kEvictRadius = 8;
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

    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup);

    u32 treeTotal() const { return instances; }

    // One placed tree. Layout mirrors tree.vert's instance attributes.
    struct Instance {
        Vec4 positionScale; // xyz = terrain point, w = uniform scale
        Vec4 params;        // x = yaw, y = tint jitter, z = sway phase, w free
    };
    // Worker output: instances bucketed per mesh variant.
    using VariantBuckets = array<vector<Instance>, kTreeVariants>;

private:
    struct Chunk {
        bool resident { false };
        rhi::BufferHandle instanceBuffer {};
        array<u32, kTreeVariants> counts {};
        array<u32, kTreeVariants> firstInstance {};
        u32 total { 0 };
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
    };

    static u64 keyOf(i32 cx, i32 cz) {
        return (static_cast<u64>(static_cast<u32>(cx)) << 32) |
               static_cast<u32>(cz);
    }

    void createVariantMeshes(rhi::Device& device, u32 terrainSeed);
    void destroyVariantMeshes(rhi::Device& device);
    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    sptr<Shared> shared;
    core::JobSystem* jobs { nullptr };

    std::unordered_map<u64, Chunk> chunks;
    u64 generation { 0 };
    u32 instances { 0 };

    array<VariantMesh, kTreeVariants> variantMeshes {};
    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
};

// Pure CPU scatter for one chunk, runs on worker threads. Deterministic.
VegetationSystem::VariantBuckets scatterTrees(const TerrainParams& params,
                                              i32 cx, i32 cz);

} // namespace render
