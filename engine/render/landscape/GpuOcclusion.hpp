#pragma once

#include <unordered_set>

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/rhi/Rhi.hpp"

namespace rhi {
class CommandBuffer;
class Device;
}

namespace render {

class ShaderLibrary;

// Hi-Z GPU occlusion culling (docs/RENDERING.md — the first compute
// user in the engine). Each frame, after the opaque pass snapshots the
// scene depth:
//   1. hiz_first/hiz_down build a half-res MAX-depth pyramid in compute
//      (image load/store — no sampler feedback),
//   2. chunk_cull tests every candidate chunk AABB against it and writes a
//      visibility word per candidate into an SSBO,
//   3. NEXT frame, the scene reads the SSBO back (the GPU finished long
//      ago — negligible stall) and drops the occluded chunks.
// One frame of latency, conservative everywhere: new candidates default
// visible, near-plane or screen-border footprints are never culled.
class GpuOcclusion {
public:
    static constexpr u32 kMaxCandidates = 4096;

    struct Candidate {
        u64 key { 0 };
        Vec3 lo {};
        Vec3 hi {};
    };

    // Requires caps: computeShaders + copyTexture (scene depth snapshot).
    void create(rhi::Device& device, ShaderLibrary& shaders);
    void destroy(rhi::Device& device);
    void refreshPipelines(rhi::Device& device, ShaderLibrary& shaders);

    // (Re)creates the pyramid for this backbuffer size (call with the
    // offscreen target's size; cheap no-op when unchanged).
    void resize(rhi::Device& device, u32 width, u32 height);

    // Reads back LAST frame's verdict into `occluded`. Call before run().
    void collectResults(rhi::Device& device,
                        std::unordered_set<u64>& occluded);

    // Records pyramid build + cull dispatch for this frame. `sceneDepth` is
    // the post-opaque depth snapshot; call outside any render pass.
    void run(rhi::CommandBuffer& cmd, rhi::Device& device,
             rhi::TextureHandle sceneDepth, const Mat4& viewProj,
             const vector<Candidate>& candidates);

    bool ready() const { return cullPipeline.id != 0 && hizTexture.id != 0; }
    u32 lastOccludedCount() const { return lastOccluded; }

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
    rhi::BindGroupHandle cullGroup {};
    rhi::TextureHandle boundDepth {}; // firstGroup references this snapshot

    rhi::BufferHandle cullUbo {};
    rhi::BufferHandle candidateBuffer {};
    rhi::BufferHandle visibilityBuffer {}; // GPU-only (stays in VRAM)
    rhi::BufferHandle stagingBuffer {};    // host-visible readback copy

    u32 hizWidth { 0 };
    u32 hizHeight { 0 };
    u32 mipCount { 0 };

    // Candidates submitted last run, matched to the readback order.
    vector<u64> pendingKeys;
    u32 lastOccluded { 0 };
    // Signals when the staging copy landed; collectResults reads only
    // then (no CPU stall), run() skips while it is pending (back-pressure).
    rhi::FenceHandle fence {};
};

} // namespace render
