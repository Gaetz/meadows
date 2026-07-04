#pragma once

#include <unordered_map>

#include "engine/core/ConcurrentQueue.hpp"
#include "engine/core/Defines.hpp"
#include "engine/render/Frustum.hpp"
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

// Animated grass (brick 14). A chunk ring around the camera scatters grass
// blades on worker threads — deterministic (seeded hash per chunk), so
// revisited meadows are identical — and draws each blade as REAL GEOMETRY:
// an instanced 5-triangle tapered ribbon, bent along its length in the
// vertex shader (per-blade lean + layered wind gusts), no alpha test at all
// (crisp silhouettes, no fizzle). Density follows the terrain material:
// blades grow only where the splat weights say grass. Same streaming
// skeleton as TerrainSystem (queue, budget, eviction, generation).
class GrassSystem {
public:
    static constexpr i32 kViewRadius = 3;  // chunks — fade hides the edge
    static constexpr i32 kEvictRadius = 4;
    static constexpr u32 kMaxUploadsPerFrame = 2;
    static constexpr f32 kFadeStart = 140.0f; // meters
    static constexpr f32 kFadeEnd = 190.0f;

    void create(rhi::Device& device, ShaderLibrary& shaders,
                core::JobSystem& jobSystem);
    void destroy(rhi::Device& device);

    // Streaming pump — main thread, once per frame (top of render).
    void update(rhi::Device& device, const TerrainParams& params,
                const Vec3& cameraPos);

    // Drops every chunk (terrain seed changed).
    void regenerate(rhi::Device& device);

    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Records instanced blade draws into the current render pass (opaque —
    // draw with the other opaques, before the sky). Grass receives shadows
    // (shadowBindGroup) but does not cast them.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle shadowBindGroup,
              const Frustum* frustum = nullptr);

    u32 instanceTotal() const { return instances; }

    // One scattered blade. Layout mirrors the grass.vert instance attributes.
    struct Instance {
        Vec4 positionScale; // xyz = terrain point, w = height scale
        Vec4 params;        // x = yaw, y = flutter phase, z = tint jitter,
                            // w = lean amount
    };

private:
    struct Chunk {
        bool resident { false };
        rhi::BufferHandle instanceBuffer {};
        u32 instanceCount { 0 };
        // Blade-root height range, for the frustum AABB.
        f32 minY { 0.0f };
        f32 maxY { 0.0f };
    };
    struct BuiltChunk {
        i32 cx { 0 };
        i32 cz { 0 };
        u64 generation { 0 };
        vector<Instance> instances;
    };
    struct Shared {
        core::ConcurrentQueue<BuiltChunk> built;
    };

    static u64 keyOf(i32 cx, i32 cz) {
        return (static_cast<u64>(static_cast<u32>(cx)) << 32) |
               static_cast<u32>(cz);
    }

    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    sptr<Shared> shared;
    core::JobSystem* jobs { nullptr };

    std::unordered_map<u64, Chunk> chunks;
    u64 generation { 0 };
    u32 instances { 0 };

    rhi::BufferHandle bladeVertexBuffer {};
    rhi::BufferHandle bladeIndexBuffer {};
    u32 bladeIndexCount { 0 };
    rhi::PipelineHandle pipeline {};
    u64 shaderGeneration { 0 };
};

// Pure CPU scatter for one chunk, runs on worker threads. Deterministic:
// the same params and coords always produce the same tufts.
vector<GrassSystem::Instance> scatterGrass(const TerrainParams& params,
                                           i32 cx, i32 cz);

} // namespace render
