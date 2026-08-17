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

// std140 mirror of froxel_inject.comp's FroxelTemporalUbo.
struct FroxelTemporalUniforms {
    Mat4 prevViewProj { 1.0f };
    Vec4 prevCamera { 0.0f };   // xyz camera, w reach (>= 2)
    Vec4 temporalInfo { 1.0f }; // x alpha, y frame, z dust wisps
};
constexpr const char* kFroxelIntegrateShader = "froxel_integrate";
constexpr const char* kFroxelApplyShader = "froxel_apply";
constexpr const char* kContactShader = "contactshadow";
constexpr const char* kSsaoShader = "ssao";
constexpr const char* kMistShader = "mist"; // ground mist raymarch
constexpr const char* kSkyCloudsShader = "skyclouds"; // volumetric clouds
constexpr const char* kCopyShader = "postcopy"; // 1:1 history blit
constexpr const char* kBlurShader = "postblur"; // contact jitter filter
constexpr const char* kLuminanceShader = "luminance"; // auto-exposure
constexpr const char* kAdaptShader = "adapt";
constexpr u32 kLuminanceSize = 64; // 7 mips -> the 1x1 log-average

} // namespace

void PostFx::create(rhi::Device& device, ShaderLibrary& shaders) {
    linearSampler = { device, device.createSampler({}) };
    nearestSampler = { device, device.createSampler(
        { .minFilter = rhi::FilterMode::Nearest,
          .magFilter = rhi::FilterMode::Nearest }) };
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
                            { { "FrameUbo", 0 }, { "LightsUbo", 5 },
                              { "FroxelTemporalUbo", 9 } },
                            { { "uShadowMap", 1 }, { "uCloudMap", 2 },
                              { "uKeyShadow", 6 }, { "uFroxelHistory", 7 },
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
        froxelScatter[0] = { device, volume() };
        froxelScatter[1] = { device, volume() };
        froxelIntegrated = { device, volume() };
        froxelTemporalUbo = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Uniform,
              .size = sizeof(FroxelTemporalUniforms),
              .dynamic = true },
            nullptr) };
        for (u32 side = 0; side < 2; ++side) {
            froxelInjectGroup[side] = { device, device.createBindGroup(
                { .entries = { { .binding = 12,
                                 .texture = froxelScatter[side].get(),
                                 .storageImage = true },
                               { .binding = 13,
                                 .texture = froxelIntegrated.get(),
                                 .storageImage = true },
                               { .binding = 7,
                                 .texture = froxelScatter[1 - side].get(),
                                 .sampler = froxelSampler.get() },
                               { .binding = 9,
                                 .buffer = froxelTemporalUbo } } }) };
        }
        froxelApplyGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 4,
                             .texture = froxelIntegrated.get(),
                             .sampler = froxelSampler.get() } } }) };
    }
    shaders.load(kContactShader, { { "FrameUbo", 0 } },
                 { { "uSceneDepth", 0 },
                   { "uShadowMap", 1 },
                   { "uSceneColor", 2 } },
                 kFullscreenVert);
    shaders.load(kSsaoShader, { { "FrameUbo", 0 } },
                 { { "uSceneDepth", 0 } }, kFullscreenVert);
    mistTemporalUbo = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          .size = sizeof(FroxelTemporalUniforms),
          .dynamic = true },
        nullptr) };
    shaders.load(kMistShader,
                 { { "FrameUbo", 0 }, { "MistTemporalUbo", 3 } },
                 { { "uSceneDepth", 0 },
                   { "uShadowMap", 1 },
                   { "uCloudMap", 2 },
                   { "uMistMap", 8 },
                   { "uNoiseVolume", 9 },
                   { "uMistHistory", 10 },
                   { "uGiCascade0", 11 } },
                 kFullscreenVert);
    skyCloudTemporalUbo = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          .size = sizeof(FroxelTemporalUniforms),
          .dynamic = true },
        nullptr) };
    shaders.load(kSkyCloudsShader,
                 { { "FrameUbo", 0 }, { "CloudTemporalUbo", 3 } },
                 { { "uSceneDepth", 0 },
                   { "uCloudMap", 2 },
                   { "uNoiseVolume", 9 },
                   { "uCloudsHistory", 10 } },
                 kFullscreenVert);
    shaders.load(kCopyShader, {}, { { "uSource", 0 } }, kFullscreenVert);
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
    // The watch derives the reload gate from the get() calls below — a
    // pass added here is watched by construction (no hand list to
    // forget it in).
    shaders.beginWatch();
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
    if (froxelScatter[0].get().id != 0) {
        froxelInjectPipeline = { device, device.createComputePipeline(
            { shaders.get(kFroxelInjectShader) }) };
        froxelIntegratePipeline = { device, device.createComputePipeline(
            { shaders.get(kFroxelIntegrateShader) }) };
        rebuild(froxelApplyPipeline, kFroxelApplyShader,
                rhi::BlendMode::Opaque);
    }
    rebuild(contactPipeline, kContactShader, rhi::BlendMode::Opaque);
    rebuild(ssaoPipeline, kSsaoShader, rhi::BlendMode::Opaque);
    rebuild(mistPipeline, kMistShader, rhi::BlendMode::Opaque);
    rebuild(skyCloudPipeline, kSkyCloudsShader, rhi::BlendMode::Opaque);
    rebuild(copyPipeline, kCopyShader, rhi::BlendMode::Opaque);
    rebuild(blurPipeline, kBlurShader, rhi::BlendMode::Opaque);
    rebuild(luminancePipeline, kLuminanceShader, rhi::BlendMode::Opaque);
    rebuild(adaptPipeline, kAdaptShader, rhi::BlendMode::Opaque);
    shaderWatch = shaders.endWatch();
}

void PostFx::refreshPipelines(rhi::Device& device, ShaderLibrary& shaders) {
    if (shaderWatch.changed(shaders)) {
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
    ssaoBlurGroup = {};
    ssaoBlurFb = {};
    ssaoBlurTex = {};
    ssaoGroup = {};
    ssaoFb = {};
    ssaoTex = {};
    mistGroup = {};
    mistFb = {};
    mistTex = {};
    mistHistoryGroup = {};
    mistHistoryFb = {};
    mistHistoryTex = {};
    skyCloudGroup = {};
    skyCloudFb = {};
    skyCloudTex = {};
    skyCloudHistoryGroup = {};
    skyCloudHistoryFb = {};
    skyCloudHistoryTex = {};
    skyCloudBlurGroup = {};
    skyCloudBlurFb = {};
    skyCloudBlurTex = {};
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
    // (godRayGroup is created below, after the sky-cloud targets exist —
    // the march composites the volumetric clouds itself.)

    makeHalfResTarget(volumetricTex, volumetricFb,
                      rhi::TextureFormat::RGBA16F);
    makeDepthGroup(volumetricGroup);

    // Ground mist — same target family as the volumetric march; its own
    // texture so the tonemap composites mist and fog in the right order
    // and the A/B toggle stays independent. History = last frame's target
    // (copied after the pass); fresh targets invalidate it.
    makeHalfResTarget(mistTex, mistFb, rhi::TextureFormat::RGBA16F);
    makeHalfResTarget(mistHistoryTex, mistHistoryFb,
                      rhi::TextureFormat::RGBA16F);
    mistHistoryGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = mistTex,
                         .sampler = linearSampler } } }) };
    mistGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneDepthCopy,
                         .sampler = linearSampler },
                       { .binding = 10,
                         .texture = mistHistoryTex.get(),
                         .sampler = linearSampler },
                       { .binding = 3,
                         .buffer = mistTemporalUbo } } }) };
    mistHistoryValid = false;

    // Volumetric sky clouds — the same target/history/temporal family.
    makeHalfResTarget(skyCloudTex, skyCloudFb, rhi::TextureFormat::RGBA16F);
    makeHalfResTarget(skyCloudHistoryTex, skyCloudHistoryFb,
                      rhi::TextureFormat::RGBA16F);
    skyCloudHistoryGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = skyCloudTex,
                         .sampler = linearSampler } } }) };
    skyCloudGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneDepthCopy,
                         .sampler = linearSampler },
                       { .binding = 10,
                         .texture = skyCloudHistoryTex.get(),
                         .sampler = linearSampler },
                       { .binding = 3,
                         .buffer = skyCloudTemporalUbo } } }) };
    makeHalfResTarget(skyCloudBlurTex, skyCloudBlurFb,
                      rhi::TextureFormat::RGBA16F);
    skyCloudBlurGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = skyCloudTex,
                         .sampler = linearSampler } } }) };
    skyCloudHistoryValid = false;
    godRayGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColorCopy,
                         .sampler = linearSampler },
                       { .binding = 1,
                         .texture = sceneDepthCopy,
                         .sampler = linearSampler } } }) };

    // Contact shadows — half-res march over the depth copy,
    // then the 3x3 blur (its IGN jitter needs the filter); the tonemap
    // taps the BLURRED target.
    makeHalfResTarget(contactTex, contactFb, rhi::TextureFormat::R16F);
    // Depth + scene color: the march reads the color ALPHA to exempt
    // grass from contact shadows on both sides (receiver early-out,
    // occluder rejection) — see contactshadow.frag. NEAREST sampling
    // on both (see nearestSampler in the header).
    contactGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneDepthCopy,
                         .sampler = nearestSampler.get() },
                       { .binding = 2,
                         .texture = sceneColorCopy,
                         .sampler = nearestSampler.get() } } }) };
    makeHalfResTarget(contactBlurTex, contactBlurFb,
                      rhi::TextureFormat::R16F);
    contactBlurGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = contactTex,
                         .sampler = linearSampler } } }) };

    // SSAO — the contact pattern's sibling: half-res Alchemy taps over
    // the depth copy, then the shared 3x3 blur (IGN jitter filter); the
    // tonemap taps the BLURRED target.
    makeHalfResTarget(ssaoTex, ssaoFb, rhi::TextureFormat::R16F);
    ssaoGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneDepthCopy,
                         .sampler = nearestSampler.get() } } }) };
    makeHalfResTarget(ssaoBlurTex, ssaoBlurFb, rhi::TextureFormat::R16F);
    ssaoBlurGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = ssaoTex,
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

void PostFx::renderSsao(rhi::CommandBuffer& cmd,
                        rhi::BindGroupHandle frameBindGroup) {
    if (ssaoTex.id() == 0) {
        return;
    }
    cmd.beginRenderPass({ .framebuffer = ssaoFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(ssaoPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, ssaoGroup);
    cmd.draw(3);
    cmd.endRenderPass();

    cmd.beginRenderPass({ .framebuffer = ssaoBlurFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(blurPipeline);
    cmd.setBindGroup(0, ssaoBlurGroup);
    cmd.draw(3);
    cmd.endRenderPass();
}

void PostFx::clearSsao(rhi::CommandBuffer& cmd) {
    if (ssaoBlurTex.id() == 0) {
        return;
    }
    cmd.beginRenderPass({ .framebuffer = ssaoBlurFb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 1.0f, 1.0f, 1.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
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

void PostFx::renderMist(rhi::Device& device, rhi::CommandBuffer& cmd,
                        const FrameUniforms& frameData,
                        rhi::BindGroupHandle frameBindGroup,
                        rhi::BindGroupHandle shadowBindGroup,
                        rhi::BindGroupHandle giApplyGroup,
                        rhi::BindGroupHandle cloudMapGroup,
                        rhi::BindGroupHandle mistMapGroup,
                        rhi::BindGroupHandle noiseVolumeGroup) {
    if (mistTex.id() == 0 || mistMapGroup.id == 0) {
        return;
    }
    // Temporal state (the froxel invalidation rules): full-weight sample
    // on the first frame, after a camera jump, or with accumulation off.
    const Vec3 camera { frameData.cameraPos };
    f32 alpha = glm::clamp(mistTemporalBlend, 0.02f, 1.0f);
    if (!mistHistoryValid ||
        glm::distance(camera, mistPrevCamera) > 10.0f) {
        alpha = 1.0f;
    }
    const FroxelTemporalUniforms temporal {
        mistPrevViewProj,
        { mistPrevCamera, 0.0f },
        { alpha, static_cast<f32>(mistFrame & 0xFFFFu), 0.0f, 0.0f }
    };
    device.updateBuffer(mistTemporalUbo, &temporal, sizeof(temporal), 0);
    ++mistFrame;
    cmd.beginRenderPass({ .framebuffer = mistFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(mistPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, mistGroup);
    if (shadowBindGroup.id != 0) {
        cmd.setBindGroup(2, shadowBindGroup);
    }
    if (giApplyGroup.id != 0) {
        cmd.setBindGroup(3, giApplyGroup);
    }
    if (noiseVolumeGroup.id != 0) {
        cmd.setBindGroup(4, noiseVolumeGroup);
    }
    // Own slots for the cloud/mist maps: PostFx overwrites slot 3 with
    // the GI group, and on Vulkan an overwritten slot loses its bindings
    // (unlike GL's sticky texture units) — never rely on a group bound
    // by an earlier pass.
    if (cloudMapGroup.id != 0) {
        cmd.setBindGroup(5, cloudMapGroup);
    }
    cmd.setBindGroup(6, mistMapGroup);
    cmd.draw(3);
    cmd.endRenderPass();
    // Next frame's history = this frame's resolved target. A draw, not
    // copyTexture: vkCmdCopyImage costs ~1 ms in MoltenVK layout
    // transitions where this blit is ~0.1 ms.
    cmd.beginRenderPass({ .framebuffer = mistHistoryFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(copyPipeline);
    cmd.setBindGroup(0, mistHistoryGroup);
    cmd.draw(3);
    cmd.endRenderPass();
    mistPrevViewProj = frameData.viewProj;
    mistPrevCamera = camera;
    mistHistoryValid = true;
}

void PostFx::clearMist(rhi::CommandBuffer& cmd) {
    if (mistTex.id() == 0) {
        return;
    }
    // Toggle-off path: neutral (no inscatter, full transmittance) — the
    // texture itself is the switch, like the contact shadows.
    cmd.beginRenderPass({ .framebuffer = mistFb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.endRenderPass();
    mistHistoryValid = false; // stale mist must not bleed back in
}

void PostFx::renderSkyClouds(rhi::Device& device, rhi::CommandBuffer& cmd,
                             const FrameUniforms& frameData,
                             rhi::BindGroupHandle frameBindGroup,
                             rhi::BindGroupHandle noiseVolumeGroup,
                             rhi::BindGroupHandle cloudMapGroup) {
    if (skyCloudTex.id() == 0 || noiseVolumeGroup.id == 0) {
        return;
    }
    const Vec3 camera { frameData.cameraPos };
    f32 alpha = glm::clamp(cloudTemporalBlend, 0.02f, 1.0f);
    if (!skyCloudHistoryValid ||
        glm::distance(camera, skyCloudPrevCamera) > 10.0f) {
        alpha = 1.0f;
    }
    const FroxelTemporalUniforms temporal {
        skyCloudPrevViewProj,
        { skyCloudPrevCamera, 0.0f },
        { alpha, static_cast<f32>(skyCloudFrame & 0xFFFFu), 0.0f, 0.0f }
    };
    device.updateBuffer(skyCloudTemporalUbo, &temporal, sizeof(temporal),
                        0);
    ++skyCloudFrame;
    cmd.beginRenderPass({ .framebuffer = skyCloudFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(skyCloudPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, skyCloudGroup);
    cmd.setBindGroup(4, noiseVolumeGroup);
    if (cloudMapGroup.id != 0) {
        cmd.setBindGroup(5, cloudMapGroup); // own slot (Vulkan rule)
    }
    cmd.draw(3);
    cmd.endRenderPass();
    // History = this frame's resolved target (postcopy blit, see mist).
    cmd.beginRenderPass({ .framebuffer = skyCloudHistoryFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(copyPipeline);
    cmd.setBindGroup(0, skyCloudHistoryGroup);
    cmd.draw(3);
    cmd.endRenderPass();
    // Display-only 3x3 blur (the froxel lesson: the tonemap taps the
    // blurred target, the EMA history above stays sharp) — soaks up the
    // residual marching variance the LOD can't.
    cmd.beginRenderPass({ .framebuffer = skyCloudBlurFb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(blurPipeline);
    cmd.setBindGroup(0, skyCloudBlurGroup);
    cmd.draw(3);
    cmd.endRenderPass();
    skyCloudPrevViewProj = frameData.viewProj;
    skyCloudPrevCamera = camera;
    skyCloudHistoryValid = true;
}

void PostFx::clearSkyClouds(rhi::CommandBuffer& cmd) {
    if (skyCloudTex.id() == 0) {
        return;
    }
    // The tonemap taps the BLURRED target — that is the one to
    // neutralize (the sharp one too, for the next history).
    cmd.beginRenderPass({ .framebuffer = skyCloudFb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.endRenderPass();
    cmd.beginRenderPass({ .framebuffer = skyCloudBlurFb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.endRenderPass();
    skyCloudHistoryValid = false;
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

void PostFx::render(rhi::Device& device, rhi::CommandBuffer& cmd,
                    const FrameUniforms& frameData,
                    rhi::BindGroupHandle frameBindGroup,
                    rhi::BindGroupHandle shadowBindGroup,
                    rhi::BindGroupHandle giApplyGroup, bool godRays,
                    GpuProbe* probe, rhi::BindGroupHandle cloudMapGroup) {
    rhi::Device* probeDevice = &device;
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
        // Temporal accumulation: the jitter decorrelates per frame and
        // the EMA against last frame's reprojected volume averages it
        // out — without this the froxel-scale noise reads as drifting
        // smoke puffs. History is invalidated on camera jumps and on
        // reach changes (interior 48 m <-> exterior 800 m), and expires
        // whenever the froxel path skips a frame.
        const Vec3 camera { frameData.cameraPos };
        const f32 reach = frameData.fogSunInfo.z;
        f32 alpha = glm::clamp(froxelTemporalBlend, 0.02f, 1.0f);
        // Reach threshold at 5 m: the reprojection is reach-aware (the
        // history slice uses prevCamera.w), so the reach drifting with a
        // weather crossfade (fogStart-driven) must NOT invalidate every
        // frame — only the interior<->exterior jump should.
        if (!froxelHistoryValid ||
            std::abs(reach - froxelPrevReach) > 5.0f ||
            glm::distance(camera, froxelPrevCamera) > 10.0f) {
            alpha = 1.0f;
        }
        const FroxelTemporalUniforms temporal {
            froxelPrevViewProj,
            { froxelPrevCamera, glm::max(froxelPrevReach, 2.0f) },
            { alpha, static_cast<f32>(froxelFrame & 0xFFFFu),
              glm::clamp(froxelDustNoise, 0.0f, 1.0f), 0.0f }
        };
        device.updateBuffer(froxelTemporalUbo, &temporal, sizeof(temporal),
                            0);
        ++froxelFrame;
        cmd.setPipeline(froxelInjectPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        if (shadowBindGroup.id != 0) {
            cmd.setBindGroup(2, shadowBindGroup);
        }
        if (giApplyGroup.id != 0) {
            cmd.setBindGroup(3, giApplyGroup);
        }
        if (cloudMapGroup.id != 0) {
            cmd.setBindGroup(5, cloudMapGroup); // own slot (Vulkan rule)
        }
        cmd.setBindGroup(4, froxelInjectGroup[froxelSide]);
        cmd.dispatch((kFroxelX + 7) / 8, (kFroxelY + 7) / 8, kFroxelZ);
        cmd.memoryBarrier(rhi::BarrierStage_Compute); // integrate imageLoads
        cmd.setPipeline(froxelIntegratePipeline);
        cmd.setBindGroup(0, frameBindGroup);
        cmd.setBindGroup(4, froxelInjectGroup[froxelSide]);
        cmd.dispatch((kFroxelX + 7) / 8, (kFroxelY + 7) / 8, 1);
        // The apply pass samples the integrated column in fragment.
        cmd.memoryBarrier(rhi::BarrierStage_Fragment);
        froxelPrevViewProj = frameData.viewProj;
        froxelPrevCamera = camera;
        froxelPrevReach = reach;
        froxelHistoryValid = frameData.time.z > 0.003f && reach > 0.0f;
        froxelSide = 1 - froxelSide;
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
        froxelHistoryValid = false; // stale volumes must not be blended
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
            // V3 (docs/RENDERING.md): the march's haze samples the RC
            // field inside its volume (giAir).
            cmd.setBindGroup(3, giApplyGroup);
        }
        if (cloudMapGroup.id != 0) {
            cmd.setBindGroup(5, cloudMapGroup); // own slot (Vulkan rule)
        }
        cmd.draw(3);
        cmd.endRenderPass();
    }
}

} // namespace render
