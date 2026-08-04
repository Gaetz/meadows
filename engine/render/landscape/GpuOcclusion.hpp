#pragma once


#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Hi-Z GPU occlusion culling (docs/RENDERING.md §6.0 — GPU-driven since
// I6, no CPU readback). Each frame, after the opaque pass snapshots the
// scene depth:
//   1. hiz_first/hiz_down build a half-res farthest-depth pyramid in
//      compute (reversed-Z: farthest = min; image load/store — no
//      sampler feedback),
//   2. chunk_cull tests every candidate AABB against it and writes one
//      DrawIndexedIndirectCommand per candidate (culled =
//      instanceCount 0) into a ping-pong command buffer,
//   3. NEXT frame, the consumers drawIndexedIndirect the per-group
//      ranges — the verdict never crosses the CPU.
// One frame of latency, conservative everywhere: near-plane straddlers
// use the frustum-plane test, screen-border footprints stay visible.
class GpuOcclusion {
public:
    // Terrain (max radius 45 -> up to ~9k resident chunks) + vegetation
    // chunk×variant entries (veg radius 24 -> up to ~31k). A candidate
    // without a command never draws, so clipping is NOT a degradation
    // mode — run() returns false and the CPU path draws everything.
    // Sized so that never happens: ~5 MB of GPU buffers.
    static constexpr u32 kMaxCandidates = 49152;
    // Draw batches: terrain LODs 0-4, vegetation (variant, level) pairs
    // above (kGroupBase + variant*3 + level). Must cover kGroupBase +
    // kVariantCount*3 — enforced by a static_assert at the vegetation
    // candidate emitter (an out-of-range group used to alias another
    // variant's batch: the whole prop blinked with the command ping-pong).
    static constexpr u32 kMaxGroups = 80;

    struct Candidate {
        Vec3 lo {};
        Vec3 hi {};
        // Indirect-draw parameters (docs/RENDERING.md §6.0): the cull
        // writes one DrawIndexedIndirectCommand per candidate, batched by
        // `group` for the consumer's per-group drawIndexedIndirect.
        // Terrain: instanceCount 1, vertexOffset = pool slot. Vegetation:
        // vertexOffset 0, firstInstance/instanceCount = the chunk's slice
        // of the pooled instance buffer.
        u32 group { 0 };
        u32 indexCount { 0 };
        i32 vertexOffset { 0 };
        u32 instanceCount { 1 };
        u32 firstInstance { 0 };
    };

    // Requires caps: computeShaders + copyTexture (scene depth snapshot).
    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);
    void refreshPipelines(rhi::Device& device, ShaderLibrary& shaders);

    // (Re)creates the pyramid for this backbuffer size (call with the
    // offscreen target's size; cheap no-op when unchanged).
    void resize(rhi::Device& device, u32 width, u32 height);

    // Records pyramid build + cull dispatch for this frame. `sceneDepth` is
    // the post-opaque depth snapshot; call outside any render pass.
    // Returns true when the indirect commands will be at most one frame
    // stale next frame — the gate for consuming them (false = the
    // candidate list clipped kMaxCandidates or the cull could not run).
    bool run(rhi::CommandBuffer& cmd, rhi::Device& device,
             rhi::TextureHandle sceneDepth, const Mat4& viewProj,
             const vector<Candidate>& candidates);

    bool ready() const { return cullPipeline.id != 0 && hizTexture.id != 0; }

    // GPU-driven consumption (no readback): the command buffer side the
    // last dispatch wrote, and its per-group ranges. Commands are consumed
    // the FRAME AFTER they were written (the draw runs before run()), so
    // the ping-pong keeps the read side stable while the write side fills.
    bool commandsValid() const { return commandsReady; }
    rhi::BufferHandle commandBuffer() const { return commandBufs[readSide]; }
    const array<u32, kMaxGroups>& groupFirst() const {
        return groupFirsts[readSide];
    }
    const array<u32, kMaxGroups>& groupCount() const {
        return groupCounts[readSide];
    }

private:
    void destroyPyramid(rhi::Device& device);

    rhi::PipelineHandle hizFirstPipeline {};
    rhi::PipelineHandle hizDownPipeline {};
    rhi::PipelineHandle cullPipeline {};
    u64 hizFirstGeneration { 0 };
    u64 hizDownGeneration { 0 };
    u64 cullGeneration { 0 };

    rhi::TextureHandle hizTexture {};
    rhi::SamplerHandle hizSampler {};   // nearest + nearest-mip: no blending
    rhi::SamplerHandle depthSampler {}; // nearest, for the first reduction
    rhi::BindGroupHandle firstGroup {};
    vector<rhi::BindGroupHandle> downGroups; // [i]: mip i -> mip i+1
    array<rhi::BindGroupHandle, 2> cullGroups {}; // one per command side
    rhi::TextureHandle boundDepth {}; // firstGroup references this snapshot

    rhi::BufferHandle cullUbo {};
    rhi::BufferHandle candidateBuffer {};
    // Ping-pong indirect command buffers: run() writes 1-readSide, the
    // NEXT frame's draw consumes it as the new readSide.
    array<rhi::BufferHandle, 2> commandBufs {};
    array<array<u32, kMaxGroups>, 2> groupFirsts {};
    array<array<u32, kMaxGroups>, 2> groupCounts {};
    u32 readSide { 0 };
    bool commandsReady { false };

    u32 hizWidth { 0 };
    u32 hizHeight { 0 };
    u32 mipCount { 0 };

};

} // namespace render
