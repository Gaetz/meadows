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
constexpr const char* kFroxelInjectShader = "froxel_inject";
constexpr const char* kFroxelIntegrateShader = "froxel_integrate";
constexpr const char* kFroxelApplyShader = "froxel_apply";
// (No screen-space AO — the sampled hemisphere
// speckles, the depth mask halos, neither fits the stepped-ramp look.
// Grounding = terrain light map + contact shadows + baked vertex AO.)
constexpr const char* kContactShader = "contactshadow";
constexpr const char* kBlurShader = "postblur"; // contact jitter filter
constexpr const char* kLuminanceShader = "luminance"; // auto-exposure
constexpr const char* kAdaptShader = "adapt";
constexpr u32 kLuminanceSize = 64; // 7 mips -> the 1x1 log-average

// Every PostFx pass shader, iterated for the generation checksum (U3-6:
// build and refresh used to spell the 9-term sum out twice — adding a
// pass needed edits in three places; this table is the single list).
constexpr const char* kPassShaders[] = {
    kPrefilterShader, kDownShader,     kUpShader,
    kGodRaysShader,   kVolumetricShader, kContactShader,
    kBlurShader,      kLuminanceShader, kAdaptShader,
    kFroxelApplyShader,
};

u64 passGenerationSum(ShaderLibrary& shaders) {
    u64 sum = 0;
    for (const char* name : kPassShaders) {
        sum += shaders.generation(name);
    }
    return sum;
}
} // namespace

void PostFx::create(rhi::Device& device, ShaderLibrary& shaders) {
    linearSampler = { device, device.createSampler({}) };
    shaders.load(kPrefilterShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kDownShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kUpShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kGodRaysShader, { { "FrameUbo", 0 } },
                 { { "uSceneColor", 0 }, { "uSceneDepth", 1 } },
                 kFullscreenVert);
    shaders.load(kVolumetricShader, { { "FrameUbo", 0 } },
                 { { "uSceneDepth", 0 }, { "uShadowMap", 1 },
                   { "uGiCascade0", 11 } },
                 kFullscreenVert);
    if (/* froxels need compute + volume textures */
        device.caps().computeShaders && device.caps().volumeTextures) {
        shaders.loadCompute(kFroxelInjectShader,
                            { { "FrameUbo", 0 }, { "LightsUbo", 5 } },
                            { { "uShadowMap", 1 }, { "uCloudMap", 2 },
                              { "uGiCascade0", 11 } });
        shaders.loadCompute(kFroxelIntegrateShader, { { "FrameUbo", 0 } });
        shaders.load(kFroxelApplyShader, { { "FrameUbo", 0 } },
                     { { "uSceneDepth", 0 }, { "uFroxelIntegrated", 4 } },
                     kFullscreenVert);
        froxelSampler = { device, device.createSampler({}) }; // linear clamp
        const auto volume = [&] {
            // Storage use is implied for RGBA16F (the RC volume pattern).
            return device.createTexture(
                { .width = kFroxelX,
                  .height = kFroxelY,
                  .depth = kFroxelZ,
                  .format = rhi::TextureFormat::RGBA16F,
                  .filter = rhi::FilterMode::Linear },
                nullptr);
        };
        froxelScatter = { device, volume() };
        froxelIntegrated = { device, volume() };
        froxelInjectGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 12,
                             .texture = froxelScatter.get(),
                             .storageImage = true },
                           { .binding = 13,
                             .texture = froxelIntegrated.get(),
                             .storageImage = true } } }) };
        froxelApplyGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 4,
                             .texture = froxelIntegrated.get(),
                             .sampler = froxelSampler.get() } } }) };
    }
    shaders.load(kContactShader, { { "FrameUbo", 0 } },
                 { { "uSceneDepth", 0 }, { "uShadowMap", 1 } },
                 kFullscreenVert);
    shaders.load(kBlurShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kLuminanceShader, {}, { { "uSceneColor", 0 } },
                 kFullscreenVert);
    shaders.load(kAdaptShader, { { "FrameUbo", 0 } },
                 { { "uLuminance", 0 }, { "uPrevExposure", 1 } },
                 kFullscreenVert);
    buildPipelines(device, shaders);
}

void PostFx::destroy(rhi::Device& device) {
    (void)device; // U3-7: every handle is rhi::Unique — the move-assign
    *this = PostFx {}; // frees them all through their device
}

void PostFx::buildPipelines(rhi::Device& device, ShaderLibrary& shaders) {
    const auto rebuild = [&](rhi::UniquePipeline& pipeline, const char* name,
                             rhi::BlendMode blend) {
        // U3-7: the assignment frees the previous pipeline.
        pipeline = { device, device.createPipeline(
                                 { .shader = shaders.get(name),
                                   .blend = blend }) };
    };
    rebuild(prefilterPipeline, kPrefilterShader, rhi::BlendMode::Opaque);
    rebuild(downPipeline, kDownShader, rhi::BlendMode::Opaque);
    rebuild(upPipeline, kUpShader, rhi::BlendMode::Additive);
    rebuild(godRayPipeline, kGodRaysShader, rhi::BlendMode::Opaque);
    rebuild(volumetricPipeline, kVolumetricShader, rhi::BlendMode::Opaque);
    if (froxelScatter.get().id != 0) {
        froxelInjectPipeline = { device, device.createComputePipeline(
            { shaders.get(kFroxelInjectShader) }) };
        froxelIntegratePipeline = { device, device.createComputePipeline(
            { shaders.get(kFroxelIntegrateShader) }) };
        rebuild(froxelApplyPipeline, kFroxelApplyShader,
                rhi::BlendMode::Opaque);
    }
    rebuild(contactPipeline, kContactShader, rhi::BlendMode::Opaque);
    rebuild(blurPipeline, kBlurShader, rhi::BlendMode::Opaque);
    rebuild(luminancePipeline, kLuminanceShader, rhi::BlendMode::Opaque);
    rebuild(adaptPipeline, kAdaptShader, rhi::BlendMode::Opaque);
    shaderGeneration = passGenerationSum(shaders);
}

void PostFx::refreshPipelines(rhi::Device& device, ShaderLibrary& shaders) {
    if (passGenerationSum(shaders) != shaderGeneration) {
        buildPipelines(device, shaders);
    }
}

void PostFx::destroyTargets(rhi::Device& device) {
    (void)device; // U3-7: assignment frees through the wrapper
    for (u32 i = 0; i < 2; ++i) {
        adaptGroup[i] = {};
        adaptFb[i] = {};
        adaptTex[i] = {};
    }
    luminanceGroup = {};
    luminanceFb = {};
    luminanceTex = {};
    contactBlurGroup = {};
    contactBlurFb = {};
    contactBlurTex = {};
    contactGroup = {};
    contactFb = {};
    contactTex = {};
    volumetricGroup = {};
    volumetricFb = {};
    volumetricTex = {};
    godRayGroup = {};
    godRayFb = {};
    godRayTex = {};
    prefilterGroup = {};
    for (u32 i = 0; i < kBloomLevels; ++i) {
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
        bloomTex[i] = { device, device.createTexture(
            { .width = levelWidth,
              .height = levelHeight,
              .format = rhi::TextureFormat::RGBA16F,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr) };
        bloomFb[i] = { device, device.createFramebuffer(
            { .colorAttachments = { { .texture = bloomTex[i] } } }) };
        levelWidth = std::max(levelWidth / 2, 1u);
        levelHeight = std::max(levelHeight / 2, 1u);
    }
    prefilterGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColor,
                         .sampler = linearSampler } } }) };
    for (u32 i = 1; i < kBloomLevels; ++i) {
        downGroup[i] = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = bloomTex[i - 1],
                             .sampler = linearSampler } } }) };
    }
    for (u32 i = 0; i + 1 < kBloomLevels; ++i) {
        upGroup[i] = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = bloomTex[i + 1],
                             .sampler = linearSampler } } }) };
    }

    // U3-6: the four half-res post targets differ only in format; their
    // sampling groups all tap the scene depth copy (god rays add the color
    // copy). One helper pair replaces four hand-copied blocks.
    const auto makeHalfResTarget = [&](rhi::UniqueTexture& tex,
                                       rhi::UniqueFramebuffer& fb,
                                       rhi::TextureFormat format) {
        tex = { device, device.createTexture(
                            { .width = std::max(width / 2, 1u),
                              .height = std::max(height / 2, 1u),
                              .format = format,
                              .filter = rhi::FilterMode::Linear,
                              .usage = rhi::TextureUsage_Sampled |
                                       rhi::TextureUsage_RenderAttachment },
                            nullptr) };
        fb = { device, device.createFramebuffer(
                           { .colorAttachments = { { .texture = tex } } }) };
    };
    const auto makeDepthGroup = [&](rhi::UniqueBindGroup& group) {
        group = { device, device.createBindGroup(
                              { .entries = { { .binding = 0,
                                               .texture = sceneDepthCopy,
                                               .sampler = linearSampler } } }) };
    };

    makeHalfResTarget(godRayTex, godRayFb, rhi::TextureFormat::RGBA16F);
    godRayGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColorCopy,
                         .sampler = linearSampler },
                       { .binding = 1,
                         .texture = sceneDepthCopy,
                         .sampler = linearSampler } } }) };

    makeHalfResTarget(volumetricTex, volumetricFb,
                      rhi::TextureFormat::RGBA16F);
    makeDepthGroup(volumetricGroup);

    // Contact shadows — half-res march over the depth copy,
    // then the 3x3 blur (its IGN jitter needs the filter); the tonemap
    // taps the BLURRED target.
    makeHalfResTarget(contactTex, contactFb, rhi::TextureFormat::R16F);
    makeDepthGroup(contactGroup);
    makeHalfResTarget(contactBlurTex, contactBlurFb,
                      rhi::TextureFormat::R16F);
    contactBlurGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = contactTex,
                         .sampler = linearSampler } } }) };

    // Auto-exposure — fixed 64² log-luminance pyramid + the two
    // 1×1 adaptation targets (ping-pong; adapt.frag snaps when the prev
    // side reads 0, so fresh targets need no seeding).
    luminanceTex = { device, device.createTexture(
        { .width = kLuminanceSize,
          .height = kLuminanceSize,
          .mipLevels = 7, // 64 -> 1
          .format = rhi::TextureFormat::R16F,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr) };
    luminanceFb = { device, device.createFramebuffer(
        { .colorAttachments = { { .texture = luminanceTex } } }) };
    luminanceGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColor,
                         .sampler = linearSampler } } }) };
    for (u32 i = 0; i < 2; ++i) {
        adaptTex[i] = { device, device.createTexture(
            { .width = 1,
              .height = 1,
              .format = rhi::TextureFormat::R16F,
              .filter = rhi::FilterMode::Nearest,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr) };
        adaptFb[i] = { device, device.createFramebuffer(
            { .colorAttachments = { { .texture = adaptTex[i] } } }) };
    }
    for (u32 i = 0; i < 2; ++i) {
        adaptGroup[i] = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = luminanceTex,
                             .sampler = linearSampler },
                           { .binding = 1,
                             .texture = adaptTex[1 - i],
                             .sampler = linearSampler } } }) };
    }
    adaptSide = 0;
}

void PostFx::renderContactShadows(rhi::CommandBuffer& cmd,
                                  rhi::BindGroupHandle frameBindGroup,
                                  rhi::BindGroupHandle shadowBindGroup) {
    if (contactTex.id() == 0) {
        return;
    }
    cmd.beginRenderPass({ .framebuffer = contactFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(contactPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    if (shadowBindGroup.id != 0) {
        // The pass reads the CSM so contact and sun
        // shadows COMBINE AS A MAX instead of multiplying (no double
        // darkening at shadowed feet) — see contactshadow.frag.
        cmd.setBindGroup(2, shadowBindGroup);
    }
    cmd.setBindGroup(1, contactGroup);
    cmd.draw(3);
    cmd.endRenderPass();

    // Speckle fix: filter the marching jitter (the tonemap taps the
    // blurred target).
    cmd.beginRenderPass({ .framebuffer = contactBlurFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(blurPipeline);
    cmd.setBindGroup(0, contactBlurGroup);
    cmd.draw(3);
    cmd.endRenderPass();
}

void PostFx::clearContactShadows(rhi::CommandBuffer& cmd) {
    if (contactBlurTex.id() == 0) {
        return;
    }
    // Toggle-off path: neutral white (there is no free FrameUbo slot for
    // a flag — the texture itself is the switch). The tonemap taps the
    // BLURRED target, so that is the one to neutralize.
    cmd.beginRenderPass({ .framebuffer = contactBlurFb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 1.0f, 1.0f, 1.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.endRenderPass();
}

void PostFx::renderAutoExposure(rhi::Device& device, rhi::CommandBuffer& cmd,
                                rhi::BindGroupHandle frameBindGroup) {
    if (luminanceTex.id() == 0) {
        return;
    }
    // 1. Log-luminance of the HDR scene into the 64² base level.
    cmd.beginRenderPass({ .framebuffer = luminanceFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(luminancePipeline);
    cmd.setBindGroup(0, luminanceGroup);
    cmd.draw(3);
    cmd.endRenderPass();

    // 2. Reduce to the 1×1 log-average (mip 6).
    device.generateMipmaps(luminanceTex);

    // 3. Adaptation micro-pass onto the other ping-pong side.
    adaptSide = 1 - adaptSide;
    cmd.beginRenderPass({ .framebuffer = adaptFb[adaptSide],
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(adaptPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, adaptGroup[adaptSide]);
    cmd.draw(3);
    cmd.endRenderPass();
}

void PostFx::render(rhi::CommandBuffer& cmd,
                    rhi::BindGroupHandle frameBindGroup,
                    rhi::BindGroupHandle shadowBindGroup,
                    rhi::BindGroupHandle giApplyGroup, bool godRays,
                    rhi::Device* probeDevice, GpuProbe* probe) {
    if (!ready()) {
        return;
    }
    {
        GpuProbe::Scope scope { probe, probeDevice, "bloom" };
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
    }

    // Skipped at intensity zero: the tonemap multiplies the (then stale)
    // texture by uSunScreen.w = 0, so a real off costs nothing.
    if (godRays) {
        GpuProbe::Scope scope { probe, probeDevice, "godrays" };
        // God rays from the pre-water scene snapshot.
        cmd.beginRenderPass({ .framebuffer = godRayFb,
                              .loadOp = rhi::LoadOp::DontCare,
                              .depthLoadOp = rhi::LoadOp::DontCare });
        cmd.setPipeline(godRayPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, godRayGroup);
        cmd.draw(3);
        cmd.endRenderPass();
    }

    if (froxelFog && froxelReady()) {
        // V4/H4 froxel path: inject + integrate in compute, then a
        // one-fetch resolve into the SAME volumetric target.
        GpuProbe::Scope scope { probe, probeDevice, "volumetric" };
        cmd.setPipeline(froxelInjectPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        if (shadowBindGroup.id != 0) {
            cmd.setBindGroup(2, shadowBindGroup);
        }
        if (giApplyGroup.id != 0) {
            cmd.setBindGroup(3, giApplyGroup);
        }
        cmd.setBindGroup(4, froxelInjectGroup);
        cmd.dispatch((kFroxelX + 7) / 8, (kFroxelY + 7) / 8, kFroxelZ);
        cmd.memoryBarrier();
        cmd.setPipeline(froxelIntegratePipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(4, froxelInjectGroup);
        cmd.dispatch((kFroxelX + 7) / 8, (kFroxelY + 7) / 8, 1);
        cmd.memoryBarrier();
        cmd.beginRenderPass({ .framebuffer = volumetricFb,
                              .loadOp = rhi::LoadOp::DontCare,
                              .depthLoadOp = rhi::LoadOp::DontCare });
        cmd.setPipeline(froxelApplyPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, volumetricGroup);
        cmd.setBindGroup(4, froxelApplyGroup);
        cmd.draw(3);
        cmd.endRenderPass();
    } else {
        GpuProbe::Scope scope { probe, probeDevice, "volumetric" };
        // The 2D march fallback: same output semantics, CSM-carved air.
        cmd.beginRenderPass({ .framebuffer = volumetricFb,
                              .loadOp = rhi::LoadOp::DontCare,
                              .depthLoadOp = rhi::LoadOp::DontCare });
        cmd.setPipeline(volumetricPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(1, volumetricGroup);
        if (shadowBindGroup.id != 0) {
            cmd.setBindGroup(2, shadowBindGroup);
        }
        if (giApplyGroup.id != 0) {
            // V3 (docs/VOLUMETRIC.md): the march's haze samples the RC
            // field inside its volume (giAir).
            cmd.setBindGroup(3, giApplyGroup);
        }
        cmd.draw(3);
        cmd.endRenderPass();
    }

    // (No screen-space AO — the tonemap does not tap an AO texture.)
}

} // namespace render
