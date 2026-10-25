#include "engine/render/landscape/SsdmChain.hpp"

#include <algorithm>

#include <glm/glm.hpp>

#include "engine/render/PipelinePresets.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {
constexpr const char* kFullscreenVert = "fullscreen";
constexpr const char* kFlowShader = "ssdm_flow";
constexpr const char* kBounds0Shader = "ssdm_bounds0";
constexpr const char* kBoundsDownShader = "ssdm_bounds_down";
constexpr const char* kResolveShader = "ssdm_resolve";
constexpr const char* kResolveHalfShader = "ssdm_resolve_half";
constexpr const char* kUpsampleShader = "ssdm_upsample";
} // namespace

void SsdmChain::create(rhi::Device& device, ShaderLibrary& shaders) {
    (void)device;
    shaders.load(kFlowShader, { render::kFrameUbo },
                 { { "uSceneColor", 0 }, { "uSceneDepth", 1 } },
                 kFullscreenVert);
    shaders.load(kBounds0Shader, { render::kFrameUbo }, { { "uFlow", 0 } },
                 kFullscreenVert);
    shaders.load(kBoundsDownShader, { render::kFrameUbo },
                 { { "uPrev", 0 } }, kFullscreenVert);
    shaders.load(kResolveHalfShader, { render::kFrameUbo },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uFlow", 2 },
                   { "uBounds0", 3 },
                   { "uBounds1", 4 },
                   { "uBounds2", 5 },
                   { "uBounds3", 6 },
                   { "uBounds4", 7 } },
                 kFullscreenVert);
    shaders.load(kUpsampleShader, { render::kFrameUbo },
                 { { "uHalf", 0 }, { "uSceneColor", 1 } }, kFullscreenVert);
    shaders.load(kResolveShader, { render::kFrameUbo },
                 { { "uSceneColor", 0 },
                   { "uSceneDepth", 1 },
                   { "uFlow", 2 },
                   { "uBounds0", 3 },
                   { "uBounds1", 4 },
                   { "uBounds2", 5 },
                   { "uBounds3", 6 },
                   { "uBounds4", 7 } },
                 kFullscreenVert);
}

void SsdmChain::destroy(rhi::Device& device) {
    // Every handle is an rhi::Unique: the resets free NOW, while the
    // device is alive (the Unique lifetime contract).
    destroyTargets(device);
    flowPipeline.reset();
    bounds0Pipeline.reset();
    downPipeline.reset();
    resolvePipeline.reset();
    resolveHalfPipeline.reset();
    upsamplePipeline.reset();
}

void SsdmChain::resize(rhi::Device& device, i32 mode, u32 width, u32 height,
                       rhi::TextureHandle sceneColorCopy,
                       rhi::TextureHandle sceneDepthCopy,
                       rhi::SamplerHandle sampler) {
    const u32 chainW = mode == kFull   ? width
                       : mode == kHalf ? glm::max(width / 2, 8u)
                                       : 4u;
    const u32 chainH = mode == kFull   ? height
                       : mode == kHalf ? glm::max(height / 2, 8u)
                                       : 4u;
    flowTex = { device, device.createTexture(
        { .width = chainW,
          .height = chainH,
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Nearest,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr) };
    flowFb = { device, device.createFramebuffer(
        { .colorAttachments = { { .texture = flowTex.get() } } }) };
    flowGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColorCopy,
                         .sampler = sampler },
                       { .binding = 1,
                         .texture = sceneDepthCopy,
                         .sampler = sampler } } }) };
    u32 levelW = chainW;
    u32 levelH = chainH;
    for (u32 i = 0; i < kLevels; ++i) {
        boundsTex[i] = { device, device.createTexture(
            { .width = levelW,
              .height = levelH,
              .format = rhi::TextureFormat::RGBA16F,
              .filter = rhi::FilterMode::Nearest,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr) };
        boundsFb[i] = { device, device.createFramebuffer(
            { .colorAttachments = { { .texture = boundsTex[i].get() } } }) };
        boundsGroup[i] = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = i == 0 ? flowTex.get()
                                               : boundsTex[i - 1].get(),
                             .sampler = sampler } } }) };
        levelW = std::max(levelW / 2, 1u);
        levelH = std::max(levelH / 2, 1u);
    }
    resolveGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColorCopy,
                         .sampler = sampler },
                       { .binding = 1,
                         .texture = sceneDepthCopy,
                         .sampler = sampler },
                       { .binding = 2,
                         .texture = flowTex.get(),
                         .sampler = sampler },
                       { .binding = 3,
                         .texture = boundsTex[0].get(),
                         .sampler = sampler },
                       { .binding = 4,
                         .texture = boundsTex[1].get(),
                         .sampler = sampler },
                       { .binding = 5,
                         .texture = boundsTex[2].get(),
                         .sampler = sampler },
                       { .binding = 6,
                         .texture = boundsTex[3].get(),
                         .sampler = sampler },
                       { .binding = 7,
                         .texture = boundsTex[4].get(),
                         .sampler = sampler } } }) };
    halfTex = { device, device.createTexture(
        { .width = chainW,
          .height = chainH,
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr) };
    halfFb = { device, device.createFramebuffer(
        { .colorAttachments = { { .texture = halfTex.get() } } }) };
    upsampleGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = halfTex.get(),
                         .sampler = sampler },
                       { .binding = 1,
                         .texture = sceneColorCopy,
                         .sampler = sampler } } }) };
}

void SsdmChain::destroyTargets(rhi::Device& device) {
    (void)device; // the Unique wrappers free through their device
    upsampleGroup.reset();
    halfFb.reset();
    halfTex.reset();
    resolveGroup.reset();
    for (u32 i = 0; i < kLevels; ++i) {
        boundsGroup[i].reset();
        boundsFb[i].reset();
        boundsTex[i].reset();
    }
    flowGroup.reset();
    flowFb.reset();
    flowTex.reset();
}

void SsdmChain::ensurePipelines(rhi::Device& device,
                                const ShaderLibrary& shaders) {
    // Six fullscreen passes sharing one state, each watching its own
    // program.
    const auto pipe = [&](WatchedPipeline& p, const char* name) {
        p.ensure(device, shaders, [&] {
            return device.createPipeline({ .shader = shaders.get(name),
                                           .blend = rhi::BlendMode::Opaque });
        });
    };
    pipe(flowPipeline, kFlowShader);
    pipe(bounds0Pipeline, kBounds0Shader);
    pipe(downPipeline, kBoundsDownShader);
    pipe(resolvePipeline, kResolveShader);
    pipe(resolveHalfPipeline, kResolveHalfShader);
    pipe(upsamplePipeline, kUpsampleShader);
}

void SsdmChain::record(rhi::Device& device, rhi::CommandBuffer& cmd,
                       const ShaderLibrary& shaders, i32 mode,
                       rhi::FramebufferHandle sceneFb,
                       rhi::BindGroupHandle frameBindGroup) {
    ensurePipelines(device, shaders);
    const auto fullscreen = [&](rhi::FramebufferHandle fb,
                                rhi::PipelineHandle pipeline,
                                rhi::BindGroupHandle group) {
        cmd.beginRenderPass({ .framebuffer = fb,
                              .loadOp = rhi::LoadOp::DontCare,
                              .depthLoadOp = rhi::LoadOp::DontCare });
        cmd.setPipeline(pipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, group);
        cmd.draw(3);
        cmd.endRenderPass();
    };
    fullscreen(flowFb.get(), flowPipeline.get(), flowGroup.get());
    fullscreen(boundsFb[0].get(), bounds0Pipeline.get(),
               boundsGroup[0].get());
    for (u32 i = 1; i < kLevels; ++i) {
        fullscreen(boundsFb[i].get(), downPipeline.get(),
                   boundsGroup[i].get());
    }
    if (mode == kHalf && halfFb.id() != 0) {
        // Half mode: resolve at chain res into the intermediate
        // (alpha = "this pixel moved"), then the edge-aware upsample
        // rewrites ONLY those pixels at full res.
        cmd.beginRenderPass({ .framebuffer = halfFb.get(),
                              .loadOp = rhi::LoadOp::DontCare,
                              .depthLoadOp = rhi::LoadOp::DontCare });
        cmd.setPipeline(resolveHalfPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, resolveGroup);
        cmd.draw(3);
        cmd.endRenderPass();
        cmd.beginRenderPass({ .framebuffer = sceneFb,
                              .loadOp = rhi::LoadOp::Load,
                              .depthLoadOp = rhi::LoadOp::Load });
        cmd.setPipeline(upsamplePipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, upsampleGroup.get());
        cmd.draw(3);
        cmd.endRenderPass();
    } else {
        cmd.beginRenderPass({ .framebuffer = sceneFb,
                              .loadOp = rhi::LoadOp::Load,
                              .depthLoadOp = rhi::LoadOp::Load });
        cmd.setPipeline(resolvePipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, resolveGroup);
        cmd.draw(3);
        cmd.endRenderPass();
    }
}

} // namespace render
