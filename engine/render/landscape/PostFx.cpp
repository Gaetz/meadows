#include "engine/render/landscape/PostFx.hpp"

#include <algorithm>

#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {
constexpr const char* kFullscreenVert = "fullscreen";
constexpr const char* kPrefilterShader = "bloom_prefilter";
constexpr const char* kDownShader = "bloom_down";
constexpr const char* kUpShader = "bloom_up";
constexpr const char* kGodRaysShader = "godrays";
constexpr const char* kVolumetricShader = "volumetric";
} // namespace

void PostFx::create(rhi::Device& device, ShaderLibrary& shaders) {
    linearSampler = device.createSampler({});
    shaders.load(kPrefilterShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kDownShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kUpShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kGodRaysShader, { { "FrameUbo", 0 } },
                 { { "uSceneColor", 0 }, { "uSceneDepth", 1 } },
                 kFullscreenVert);
    shaders.load(kVolumetricShader, { { "FrameUbo", 0 } },
                 { { "uSceneDepth", 0 }, { "uShadowMap", 1 } },
                 kFullscreenVert);
    buildPipelines(device, shaders);
}

void PostFx::destroy(rhi::Device& device) {
    destroyTargets(device);
    device.destroyPipeline(volumetricPipeline);
    device.destroyPipeline(godRayPipeline);
    device.destroyPipeline(upPipeline);
    device.destroyPipeline(downPipeline);
    device.destroyPipeline(prefilterPipeline);
    device.destroySampler(linearSampler);
    *this = PostFx {};
}

void PostFx::buildPipelines(rhi::Device& device, ShaderLibrary& shaders) {
    const auto rebuild = [&](rhi::PipelineHandle& pipeline, const char* name,
                             rhi::BlendMode blend) {
        if (pipeline.id != 0) {
            device.destroyPipeline(pipeline);
        }
        pipeline = device.createPipeline(
            { .shader = shaders.get(name), .blend = blend });
    };
    rebuild(prefilterPipeline, kPrefilterShader, rhi::BlendMode::Opaque);
    rebuild(downPipeline, kDownShader, rhi::BlendMode::Opaque);
    rebuild(upPipeline, kUpShader, rhi::BlendMode::Additive);
    rebuild(godRayPipeline, kGodRaysShader, rhi::BlendMode::Opaque);
    rebuild(volumetricPipeline, kVolumetricShader, rhi::BlendMode::Opaque);
    shaderGeneration = shaders.generation(kPrefilterShader) +
                       shaders.generation(kDownShader) +
                       shaders.generation(kUpShader) +
                       shaders.generation(kGodRaysShader) +
                       shaders.generation(kVolumetricShader);
}

void PostFx::refreshPipelines(rhi::Device& device, ShaderLibrary& shaders) {
    const u64 current = shaders.generation(kPrefilterShader) +
                        shaders.generation(kDownShader) +
                        shaders.generation(kUpShader) +
                        shaders.generation(kGodRaysShader) +
                        shaders.generation(kVolumetricShader);
    if (current != shaderGeneration) {
        buildPipelines(device, shaders);
    }
}

void PostFx::destroyTargets(rhi::Device& device) {
    device.destroyBindGroup(volumetricGroup);
    device.destroyFramebuffer(volumetricFb);
    device.destroyTexture(volumetricTex);
    volumetricGroup = {};
    volumetricFb = {};
    volumetricTex = {};
    device.destroyBindGroup(godRayGroup);
    device.destroyFramebuffer(godRayFb);
    device.destroyTexture(godRayTex);
    godRayGroup = {};
    godRayFb = {};
    godRayTex = {};
    device.destroyBindGroup(prefilterGroup);
    prefilterGroup = {};
    for (u32 i = 0; i < kBloomLevels; ++i) {
        device.destroyBindGroup(downGroup[i]);
        device.destroyBindGroup(upGroup[i]);
        device.destroyFramebuffer(bloomFb[i]);
        device.destroyTexture(bloomTex[i]);
        downGroup[i] = {};
        upGroup[i] = {};
        bloomFb[i] = {};
        bloomTex[i] = {};
    }
}

void PostFx::resize(rhi::Device& device, u32 width, u32 height,
                    rhi::TextureHandle sceneColor,
                    rhi::TextureHandle sceneColorCopy,
                    rhi::TextureHandle sceneDepthCopy) {
    destroyTargets(device);

    u32 levelWidth = std::max(width / 2, 1u);
    u32 levelHeight = std::max(height / 2, 1u);
    for (u32 i = 0; i < kBloomLevels; ++i) {
        bloomTex[i] = device.createTexture(
            { .width = levelWidth,
              .height = levelHeight,
              .format = rhi::TextureFormat::RGBA16F,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr);
        bloomFb[i] = device.createFramebuffer(
            { .colorAttachments = { { .texture = bloomTex[i] } } });
        levelWidth = std::max(levelWidth / 2, 1u);
        levelHeight = std::max(levelHeight / 2, 1u);
    }
    prefilterGroup = device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColor,
                         .sampler = linearSampler } } });
    for (u32 i = 1; i < kBloomLevels; ++i) {
        downGroup[i] = device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = bloomTex[i - 1],
                             .sampler = linearSampler } } });
    }
    for (u32 i = 0; i + 1 < kBloomLevels; ++i) {
        upGroup[i] = device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = bloomTex[i + 1],
                             .sampler = linearSampler } } });
    }

    godRayTex = device.createTexture(
        { .width = std::max(width / 2, 1u),
          .height = std::max(height / 2, 1u),
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr);
    godRayFb = device.createFramebuffer(
        { .colorAttachments = { { .texture = godRayTex } } });
    godRayGroup = device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColorCopy,
                         .sampler = linearSampler },
                       { .binding = 1,
                         .texture = sceneDepthCopy,
                         .sampler = linearSampler } } });

    volumetricTex = device.createTexture(
        { .width = std::max(width / 2, 1u),
          .height = std::max(height / 2, 1u),
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr);
    volumetricFb = device.createFramebuffer(
        { .colorAttachments = { { .texture = volumetricTex } } });
    volumetricGroup = device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneDepthCopy,
                         .sampler = linearSampler } } });
}

void PostFx::render(rhi::CommandBuffer& cmd,
                    rhi::BindGroupHandle frameBindGroup,
                    rhi::BindGroupHandle shadowBindGroup) {
    if (!ready()) {
        return;
    }
    // Prefilter: HDR highlights into level 0.
    cmd.beginRenderPass({ .framebuffer = bloomFb[0],
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(prefilterPipeline);
    cmd.setBindGroup(0, prefilterGroup);
    cmd.draw(3);
    cmd.endRenderPass();

    // Down the pyramid.
    for (u32 i = 1; i < kBloomLevels; ++i) {
        cmd.beginRenderPass({ .framebuffer = bloomFb[i],
                              .loadOp = rhi::LoadOp::DontCare,
                              .depthLoadOp = rhi::LoadOp::DontCare });
        cmd.setPipeline(downPipeline);
        cmd.setBindGroup(0, downGroup[i]);
        cmd.draw(3);
        cmd.endRenderPass();
    }

    // Back up, additively widening the glow.
    for (i32 i = static_cast<i32>(kBloomLevels) - 2; i >= 0; --i) {
        cmd.beginRenderPass({ .framebuffer = bloomFb[i],
                              .loadOp = rhi::LoadOp::Load,
                              .depthLoadOp = rhi::LoadOp::DontCare });
        cmd.setPipeline(upPipeline);
        cmd.setBindGroup(0, upGroup[i]);
        cmd.draw(3);
        cmd.endRenderPass();
    }

    // God rays from the pre-water scene snapshot.
    cmd.beginRenderPass({ .framebuffer = godRayFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(godRayPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, godRayGroup);
    cmd.draw(3);
    cmd.endRenderPass();

    // Volumetric shafts: march the air, carved by clouds and CSM geometry.
    cmd.beginRenderPass({ .framebuffer = volumetricFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(volumetricPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, volumetricGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    cmd.draw(3);
    cmd.endRenderPass();
}

} // namespace render
