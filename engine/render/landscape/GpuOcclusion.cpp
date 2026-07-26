#include "engine/render/landscape/GpuOcclusion.hpp"

#include <algorithm>

#include <glm/glm.hpp>

#include "engine/core/Log.hpp"
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
    Vec4 info {}; // x = count, y = mips, zw = Hi-Z base size
};

// std430 mirror of the candidate SSBO entry.
struct GpuAabb {
    Vec4 lo {};
    Vec4 hi {};
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
    visibilityBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Storage,
          .size = kMaxCandidates * sizeof(u32) },
        nullptr);
    // Staging pattern: the SSBO stays device-local; a GPU-side copy lands
    // in this host-visible buffer, which is what the CPU reads (no per-
    // frame VRAM<->RAM migration of the working buffer).
    stagingBuffer = device.createBuffer(
        { .usage = rhi::BufferUsage::Storage,
          .size = kMaxCandidates * sizeof(u32),
          .readback = true },
        nullptr);
}

void GpuOcclusion::destroyPyramid(rhi::Device& device) {
    device.destroyBindGroup(firstGroup);
    firstGroup = {};
    for (const rhi::BindGroupHandle group : downGroups) {
        device.destroyBindGroup(group);
    }
    downGroups.clear();
    device.destroyBindGroup(cullGroup);
    cullGroup = {};
    device.destroyTexture(hizTexture);
    hizTexture = {};
    boundDepth = {};
}

void GpuOcclusion::destroy(rhi::Device& device) {
    device.destroyFence(fence); // abandon an in-flight verdict
    fence = {};
    destroyPyramid(device);
    device.destroyBuffer(stagingBuffer);
    device.destroyBuffer(visibilityBuffer);
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
    cullGroup = device.createBindGroup(
        { .entries = { { .binding = 0, .buffer = cullUbo },
                       { .binding = 1,
                         .buffer = candidateBuffer,
                         .storage = true },
                       { .binding = 2,
                         .buffer = visibilityBuffer,
                         .storage = true },
                       { .binding = 3,
                         .texture = hizTexture,
                         .sampler = hizSampler } } });
    // firstGroup depends on the depth snapshot texture: (re)built in run().
    pendingKeys.clear(); // stale visibility layout after a resize
}

void GpuOcclusion::collectResults(rhi::Device& device,
                                  std::unordered_set<u64>& occluded) {
    if (pendingKeys.empty()) {
        occluded.clear();
        lastOccluded = 0;
        return;
    }
    // The verdict buffer is read ONLY once its fence signals —
    // glGetBufferSubData on a still-in-flight buffer stalls the CPU until
    // the GPU catches up (~25 ms mainPass spikes). While pending, the
    // caller keeps the PREVIOUS verdict (occluded left untouched): the
    // occlusion set is already temporal, one extra frame is invisible.
    if (!device.fenceReady(fence)) {
        return;
    }
    fence = {};
    vector<u32> visibility(pendingKeys.size());
    device.readBuffer(stagingBuffer, visibility.data(),
                      visibility.size() * sizeof(u32), 0);
    occluded.clear();
    u32 count = 0;
    for (size_t i = 0; i < pendingKeys.size(); ++i) {
        if (visibility[i] == 0) {
            occluded.insert(pendingKeys[i]);
            ++count;
        }
    }
    lastOccluded = count;
    pendingKeys.clear();
}

void GpuOcclusion::run(rhi::CommandBuffer& cmd, rhi::Device& device,
                       rhi::TextureHandle sceneDepth, const Mat4& viewProj,
                       const vector<Candidate>& candidates) {
    if (!ready() || hizFirstPipeline.id == 0 || hizDownPipeline.id == 0 ||
        candidates.empty()) {
        return;
    }
    if (fence.id != 0) {
        // Back-pressure: the previous verdict has not been consumed yet
        // (its fence is still pending) — re-dispatching would overwrite the
        // staging buffer mid-read window. Skip; the ring re-runs next frame.
        return;
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

    // Upload this frame's candidates.
    const u32 count = static_cast<u32>(
        glm::min<size_t>(candidates.size(), kMaxCandidates));
    vector<GpuAabb> aabbs(count);
    pendingKeys.resize(count);
    for (u32 i = 0; i < count; ++i) {
        aabbs[i].lo = { candidates[i].lo, 0.0f };
        aabbs[i].hi = { candidates[i].hi, 0.0f };
        pendingKeys[i] = candidates[i].key;
    }
    device.updateBuffer(candidateBuffer, aabbs.data(),
                        count * sizeof(GpuAabb), 0);
    const CullUniforms uniforms {
        .viewProj = viewProj,
        .info = { static_cast<f32>(count), static_cast<f32>(mipCount),
                  static_cast<f32>(hizWidth), static_cast<f32>(hizHeight) },
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

    // Cull: one thread per candidate, then stage the verdict for the CPU.
    cmd.memoryBarrier(rhi::BarrierStage_Compute); // pyramid -> cull reads
    cmd.setPipeline(cullPipeline);
    cmd.setBindGroup(0, cullGroup);
    cmd.dispatch((count + 63) / 64);
    // SSBO writes visible to the staging copy only — nothing else in the
    // frame reads the verdict (the CPU does, behind the fence).
    cmd.memoryBarrier(rhi::BarrierStage_Transfer);
    cmd.copyBuffer(visibilityBuffer, stagingBuffer, count * sizeof(u32));
    // Marker after the copy — collectResults reads only once this
    // signals (never blocks the frame on the GPU catching up).
    fence = device.insertFence();
}

} // namespace render
