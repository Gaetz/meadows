#include "engine/render/landscape/GpuOcclusion.hpp"

#include <algorithm>

#include <glm/glm.hpp>

#include "engine/core/Log.hpp"
#include "engine/render/Frustum.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kHizFirstShader = "hiz_first";
constexpr const char* kHizDownShader = "hiz_down";
constexpr const char* kCullShader = "chunk_cull";

// std140 mirror of chunk_cull.comp's CullUbo.
struct CullUniforms {
    Mat4 viewProj {};
    Vec4 info {};  // x = count, y = mips, zw = Hi-Z base size
    Vec4 info2 {}; // x = write commands, y = NDC guard-band scale,
                   // z = plane margin (m) for near-straddling boxes
    array<Vec4, 6> planes {}; // the frustum, for boxes the NDC test
                              // cannot judge (near-plane straddlers)
};

u32 mipCountFor(u32 width, u32 height) {
    u32 mips = 1;
    u32 size = glm::max(width, height);
    while (size > 1) {
        size >>= 1;
        ++mips;
    }
    return mips;
}

} // namespace

void GpuOcclusion::create(rhi::Device& device, ShaderLibrary& shaders) {
    shaders.loadCompute(kHizFirstShader, {}, { { "uSceneDepth", 0 } });
    shaders.loadCompute(kHizDownShader);
    shaders.loadCompute(kCullShader, { { "CullUbo", 0 } },
                        { { "uHiZ", 3 } });
    refreshPipelines(device, shaders);

    depthSampler = device.createSampler(
        { .minFilter = rhi::FilterMode::Nearest,
          .magFilter = rhi::FilterMode::Nearest });
    hizSampler = device.createSampler(
        { .minFilter = rhi::FilterMode::Nearest,
          .magFilter = rhi::FilterMode::Nearest,
          .mipmapFilter = true }); // NEAREST_MIPMAP_NEAREST

    cullUbo = device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                    .size = sizeof(CullUniforms),
                                    .dynamic = true },
                                  nullptr);
    candidateBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Storage,
          .size = kMaxCandidates * sizeof(GpuAabb),
          .dynamic = true },
        nullptr);
    // GPU-driven: the cull writes one indirect command per candidate.
    // Ping-pong so the write never races the frame still consuming the
    // other side (draws run before the dispatch each frame).
    for (rhi::BufferHandle& buf : commandBufs) {
        buf = device.createBuffer(
            { .usage = rhi::BufferUsage::Indirect,
              .size = kMaxCandidates *
                      sizeof(rhi::DrawIndexedIndirectCommand) },
            nullptr);
    }
}

void GpuOcclusion::destroyPyramid(rhi::Device& device) {
    device.destroyBindGroup(firstGroup);
    firstGroup = {};
    for (const rhi::BindGroupHandle group : downGroups) {
        device.destroyBindGroup(group);
    }
    downGroups.clear();
    for (rhi::BindGroupHandle& group : cullGroups) {
        device.destroyBindGroup(group);
        group = {};
    }
    device.destroyTexture(hizTexture);
    hizTexture = {};
    boundDepth = {};
    commandsReady = false; // stale binding layout
}

void GpuOcclusion::destroy(rhi::Device& device) {
    destroyPyramid(device);
    for (rhi::BufferHandle& buf : commandBufs) {
        device.destroyBuffer(buf);
    }
    device.destroyBuffer(candidateBuffer);
    device.destroyBuffer(cullUbo);
    device.destroySampler(hizSampler);
    device.destroySampler(depthSampler);
    device.destroyPipeline(hizFirstPipeline);
    device.destroyPipeline(hizDownPipeline);
    device.destroyPipeline(cullPipeline);
    *this = GpuOcclusion {};
}

void GpuOcclusion::refreshPipelines(rhi::Device& device,
                                    ShaderLibrary& shaders) {
    const auto refresh = [&](const char* name, rhi::PipelineHandle& pipeline,
                             u64& generation) {
        if (shaders.generation(name) == generation) {
            return;
        }
        if (pipeline.id != 0) {
            device.destroyPipeline(pipeline);
        }
        pipeline = device.createComputePipeline({ shaders.get(name) });
        generation = shaders.generation(name);
    };
    refresh(kHizFirstShader, hizFirstPipeline, hizFirstGeneration);
    refresh(kHizDownShader, hizDownPipeline, hizDownGeneration);
    refresh(kCullShader, cullPipeline, cullGeneration);
}

void GpuOcclusion::resize(rhi::Device& device, u32 width, u32 height) {
    const u32 newWidth = glm::max(width / 2, 1u);
    const u32 newHeight = glm::max(height / 2, 1u);
    if (newWidth == hizWidth && newHeight == hizHeight &&
        hizTexture.id != 0) {
        return;
    }
    destroyPyramid(device);
    hizWidth = newWidth;
    hizHeight = newHeight;
    mipCount = mipCountFor(hizWidth, hizHeight);
    hizTexture = device.createTexture(
        { .width = hizWidth,
          .height = hizHeight,
          .mipLevels = mipCount,
          .format = rhi::TextureFormat::R32F,
          .filter = rhi::FilterMode::Nearest,
          .usage = rhi::TextureUsage_Sampled },
        nullptr);
    for (u32 mip = 0; mip + 1 < mipCount; ++mip) {
        downGroups.push_back(device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = hizTexture,
                             .storageImage = true,
                             .imageMip = mip },
                           { .binding = 1,
                             .texture = hizTexture,
                             .storageImage = true,
                             .imageMip = mip + 1 } } }));
    }
    for (u32 side = 0; side < 2; ++side) {
        cullGroups[side] = device.createBindGroup(
            { .entries = { { .binding = 0, .buffer = cullUbo },
                           { .binding = 1,
                             .buffer = candidateBuffer,
                             .storage = true },
                           { .binding = 3,
                             .texture = hizTexture,
                             .sampler = hizSampler },
                           { .binding = 4,
                             .buffer = commandBufs[side],
                             .storage = true } } });
    }
    // firstGroup depends on the depth snapshot texture: (re)built in run().
}

bool GpuOcclusion::run(rhi::CommandBuffer& cmd, rhi::Device& device,
                       rhi::TextureHandle sceneDepth, const Mat4& viewProj,
                       const vector<Candidate>& candidates) {
    if (!ready() || hizFirstPipeline.id == 0 || hizDownPipeline.id == 0 ||
        candidates.empty()) {
        return false;
    }
    if (boundDepth.id != sceneDepth.id) {
        // The depth snapshot texture changed (first run / window resize):
        // rebind the first-reduction group to it.
        device.destroyBindGroup(firstGroup);
        firstGroup = device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = sceneDepth,
                             .sampler = depthSampler },
                           { .binding = 1,
                             .texture = hizTexture,
                             .storageImage = true,
                             .imageMip = 0 } } });
        boundDepth = sceneDepth;
    }

    // Upload this frame's candidates, GROUP-SORTED so the indirect
    // commands land in contiguous per-group ranges (one
    // drawIndexedIndirect per group).
    const u32 count = static_cast<u32>(
        glm::min<size_t>(candidates.size(), kMaxCandidates));
    // A clipped list would mean chunks with NO command at all — silently
    // missing geometry. Dispatch what fits but report the commands unfit
    // (the consumers fall back to their CPU loops for the frame).
    const bool clipped = candidates.size() > kMaxCandidates;
    const u32 writeSide = 1 - readSide;
    array<u32, kMaxGroups>& firsts = groupFirsts[writeSide];
    array<u32, kMaxGroups>& counts = groupCounts[writeSide];
    counts.fill(0);
    for (u32 i = 0; i < count; ++i) {
        counts[glm::min(candidates[i].group, kMaxGroups - 1)]++;
    }
    u32 running = 0;
    for (u32 g = 0; g < kMaxGroups; ++g) {
        firsts[g] = running;
        running += counts[g];
    }
    array<u32, kMaxGroups> cursor = firsts;
    // Reused scratch: every slot below is written exactly once (the
    // cursor partition covers [0, count)), so no per-frame allocation
    // nor clear is needed — this can reach ~4 MB at full candidate load.
    aabbScratch.resize(count);
    vector<GpuAabb>& aabbs = aabbScratch;
    for (u32 i = 0; i < count; ++i) {
        const Candidate& c = candidates[i];
        const u32 slot = cursor[glm::min(c.group, kMaxGroups - 1)]++;
        aabbs[slot].lo = { c.lo, 0.0f };
        aabbs[slot].hi = { c.hi, 0.0f };
        aabbs[slot].draw = { c.indexCount,
                             static_cast<u32>(c.vertexOffset), c.group,
                             c.instanceCount };
        aabbs[slot].draw2 = { c.firstInstance, 0u, 0u, 0u };
    }
    device.updateBuffer(candidateBuffer, aabbs.data(),
                        count * sizeof(GpuAabb), 0);
    const CullUniforms uniforms {
        .viewProj = viewProj,
        .info = { static_cast<f32>(count), static_cast<f32>(mipCount),
                  static_cast<f32>(hizWidth), static_cast<f32>(hizHeight) },
        // NDC guard band 1.15 (proportional — covers the one-frame-stale
        // rotation at any distance) + a 16 m world margin for the plane
        // test that judges near-plane straddlers (they are close, so a
        // frame of motion is small in meters).
        .info2 = { 1.0f, 1.15f, 16.0f, 0.0f },
        .planes = Frustum::fromViewProj(viewProj).planes,
    };
    device.updateBuffer(cullUbo, &uniforms, sizeof(uniforms), 0);

    // Pyramid: first reduction from the depth snapshot, then mip chain.
    cmd.setPipeline(hizFirstPipeline);
    cmd.setBindGroup(0, firstGroup);
    cmd.dispatch((hizWidth + 7) / 8, (hizHeight + 7) / 8);
    cmd.setPipeline(hizDownPipeline);
    u32 mipW = hizWidth;
    u32 mipH = hizHeight;
    for (u32 mip = 0; mip + 1 < mipCount; ++mip) {
        mipW = glm::max(mipW / 2, 1u);
        mipH = glm::max(mipH / 2, 1u);
        // Level N-1 writes visible to level N reads.
        cmd.memoryBarrier(rhi::BarrierStage_Compute);
        cmd.setBindGroup(0, downGroups[mip]);
        cmd.dispatch((mipW + 7) / 8, (mipH + 7) / 8);
    }

    // Cull: one thread per candidate, writing the commands.
    cmd.memoryBarrier(rhi::BarrierStage_Compute); // pyramid -> cull reads
    cmd.setPipeline(cullPipeline);
    cmd.setBindGroup(0, cullGroups[writeSide]);
    cmd.dispatch((count + 63) / 64);
    // Command writes visible to the NEXT frame's indirect argument reads.
    cmd.memoryBarrier(rhi::BarrierStage_Indirect);
    readSide = writeSide; // consumed by the NEXT frame's draw
    commandsReady = !clipped;
    return !clipped;
}

} // namespace render
