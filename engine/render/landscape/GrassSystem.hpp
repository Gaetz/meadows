#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/Frustum.hpp"
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
    // Scatter jobs are budgeted like uploads (a border crossing used to
    // dump a whole ring edge of dense scatters on the workers at once —
    // part of the fast-travel stutter; see TerrainSystem).
    static constexpr u32 kMaxRequestsPerFrame = 2;
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

    // Drops only these chunks (cx,cz keys) so update() re-scatters them onto
    // the current terrain — the sculpt path. Non-resident chunks are left to
    // finish streaming. Keys share the terrain chunk grid (keyOf).
    void invalidateChunks(rhi::Device& device, const vector<u64>& keys);

    void refreshPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // Records instanced blade draws into the current render pass (opaque —
    // draw with the other opaques, before the sky). Grass receives shadows
    // (shadowBindGroup) but does not cast them. cameraPos drives the
    // metric density LOD: instances are sorted by the keep key grass.vert
    // clips against, so each chunk draws exactly the prefix the shader
    // keeps at its nearest distance (fewer, wider blades far away);
    // chunks are emitted front-to-back for early-z.
    void draw(rhi::CommandBuffer& cmd, rhi::BindGroupHandle frameBindGroup,
              rhi::BindGroupHandle shadowBindGroup, const Vec3& cameraPos,
              const Frustum* frustum = nullptr);

    u32 instanceTotal() const { return instances; }

    // One scattered blade. Layout mirrors the grass.vert instance attributes.
    struct Instance {
        Vec4 positionScale; // xyz = terrain point, w = height scale
        Vec4 params;        // x = yaw, y = flutter phase, z = tint jitter,
                            // w = lean amount
        // 7.8bis — THE BotW look mechanism: blades shade with the GROUND
        // normal (the meadow lights as one continuous surface; blade
        // geometry only shows in silhouettes and wind).
        Vec4 groundNormal;  // xyz = terrain normal at the root, w unused
    };

private:
    struct Chunk {
        bool resident { false };
        rhi::UniqueBuffer instanceBuffer;
        u32 instanceCount { 0 };
        // Blade-root height range, for the frustum AABB.
        f32 minY { 0.0f };
        f32 maxY { 0.0f };
    };

    void buildPipeline(rhi::Device& device, ShaderLibrary& shaders);

    // The shared ring mechanics (audit U3-1): map + generation-stamped
    // queue + budgeted request/evict live in ChunkStreamer.
    ChunkStreamer<Chunk, vector<Instance>> streamer;
    u32 instances { 0 };

    rhi::UniqueBuffer bladeVertexBuffer;
    rhi::UniqueBuffer bladeIndexBuffer;
    u32 bladeIndexCount { 0 };
    rhi::UniquePipeline pipeline;
    u64 shaderGeneration { 0 };
};

// Pure CPU scatter for one chunk, runs on worker threads. Deterministic:
// the same params and coords always produce the same tufts.
vector<GrassSystem::Instance> scatterGrass(const TerrainParams& params,
                                           i32 cx, i32 cz);

} // namespace render
