#include "engine/render/landscape/PostFx.hpp"

#include <algorithm>
#include <utility>

#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/render/PipelinePresets.hpp"

namespace render {

namespace {
constexpr const char* kFullscreenVert = "fullscreen";
constexpr const char* kPrefilterShader = "bloom_prefilter";
constexpr const char* kDownShader = "bloom_down";
constexpr const char* kUpShader = "bloom_up";
constexpr const char* kGodRaysShader = "godrays";
constexpr const char* kVolumetricShader = "volumetric";
constexpr const char* kFroxelInjectShader = "froxel_inject";

// std140 mirror of froxel_inject.comp's FroxelTemporalUbo — the mist and
// sky-cloud temporal blocks share the layout.
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

// --- Target families -----------------------------------------------------------------

void PostFx::RenderTarget::create(rhi::Device& device, u32 width, u32 height,
                                  rhi::TextureFormat format,
                                  rhi::FilterMode filter, u32 mipLevels) {
    tex = { device, device.createTexture(
                        { .width = width,
                          .height = height,
                          .mipLevels = mipLevels,
                          .format = format,
                          .filter = filter,
                          .usage = rhi::TextureUsage_Sampled |
                                   rhi::TextureUsage_RenderAttachment },
                        nullptr) };
    fb = { device, device.createFramebuffer(
                       { .colorAttachments = { { .texture = tex } } }) };
}

void PostFx::RenderTarget::reset() {
    fb.reset();
    tex.reset();
}

void PostFx::DerivedTarget::create(rhi::Device& device, u32 width, u32 height,
                                   rhi::TextureFormat format,
                                   rhi::TextureHandle sourceTex,
                                   rhi::SamplerHandle sampler) {
    target.create(device, width, height, format);
    source = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sourceTex,
                         .sampler = sampler } } }) };
}

void PostFx::DerivedTarget::reset() {
    source.reset();
    target.reset();
}

void PostFx::DerivedTarget::record(rhi::CommandBuffer& cmd,
                                   rhi::PipelineHandle pipeline) const {
    cmd.beginRenderPass({ .framebuffer = target.fb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, source);
    cmd.draw(3);
    cmd.endRenderPass();
}

void PostFx::TemporalHistory::create(rhi::Device& device) {
    ubo = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          .size = sizeof(FroxelTemporalUniforms),
          .dynamic = true },
        nullptr) };
}

f32 PostFx::TemporalHistory::alpha(f32 blend, const Vec3& camera) const {
    // Full-weight sample on the first frame, after a camera jump, or with
    // accumulation off.
    f32 weight = glm::clamp(blend, 0.02f, 1.0f);
    if (!valid || glm::distance(camera, prevCamera) > 10.0f) {
        weight = 1.0f;
    }
    return weight;
}

void PostFx::TemporalHistory::upload(rhi::Device& device, f32 alpha,
                                     f32 cameraW, f32 dustNoise) {
    const FroxelTemporalUniforms temporal {
        prevViewProj,
        { prevCamera, cameraW },
        { alpha, static_cast<f32>(frame & 0xFFFFu), dustNoise, 0.0f }
    };
    device.updateBuffer(ubo, &temporal, sizeof(temporal), 0);
    ++frame;
}

void PostFx::TemporalHistory::commit(const Mat4& viewProj, const Vec3& camera,
                                     f32 reach, bool nowValid) {
    prevViewProj = viewProj;
    prevCamera = camera;
    prevReach = reach;
    valid = nowValid;
}

void PostFx::MediaPass::createTargets(rhi::Device& device, u32 width,
                                      u32 height,
                                      rhi::TextureHandle sceneDepthCopy,
                                      rhi::SamplerHandle sampler) {
    // Fresh targets invalidate the history.
    out.create(device, width, height, rhi::TextureFormat::RGBA16F);
    history.create(device, width, height, rhi::TextureFormat::RGBA16F,
                   out.tex, sampler);
    group = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneDepthCopy,
                         .sampler = sampler },
                       { .binding = 10,
                         .texture = history.target.tex.get(),
                         .sampler = sampler },
                       { .binding = 3,
                         .buffer = temporal.ubo } } }) };
    temporal.valid = false;
}

void PostFx::MediaPass::resetTargets() {
    group.reset();
    history.reset();
    out.reset();
}

void PostFx::FilteredPass::reset() {
    blur.reset();
    group.reset();
    raw.reset();
}

// --- Lifetime --------------------------------------------------------------------------

void PostFx::create(rhi::Device& device, ShaderLibrary& shaders) {
    linearSampler = { device, device.createSampler({}) };
    nearestSampler = { device, device.createSampler(
        { .minFilter = rhi::FilterMode::Nearest,
          .magFilter = rhi::FilterMode::Nearest }) };
    shaders.load(kPrefilterShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kDownShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kUpShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kGodRaysShader, { render::kFrameUbo },
                 { { "uSceneColor", 0 }, { "uSceneDepth", 1 } },
                 kFullscreenVert);
    shaders.load(kVolumetricShader, { render::kFrameUbo },
                 { { "uSceneDepth", 0 }, { "uShadowMap", 1 },
                   { "uGiCascade0", 11 } },
                 kFullscreenVert);
    if (/* froxels need compute + volume textures */
        device.caps().computeShaders && device.caps().volumeTextures) {
        shaders.loadCompute(kFroxelInjectShader,
                            { render::kFrameUbo, render::kLightsUbo,
                              { "FroxelTemporalUbo", 9 } },
                            { { "uShadowMap", 1 }, { "uCloudMap", 2 },
                              { "uKeyShadow", 6 }, { "uFroxelHistory", 7 },
                              { "uGiCascade0", 11 } });
        shaders.loadCompute(kFroxelIntegrateShader, { render::kFrameUbo });
        shaders.load(kFroxelApplyShader, { render::kFrameUbo },
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
        froxelTemporal.create(device);
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
                                 .buffer = froxelTemporal.ubo } } }) };
        }
        froxelApplyGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 4,
                             .texture = froxelIntegrated.get(),
                             .sampler = froxelSampler.get() } } }) };
    }
    shaders.load(kContactShader, { render::kFrameUbo },
                 { { "uSceneDepth", 0 },
                   { "uShadowMap", 1 },
                   { "uSceneColor", 2 } },
                 kFullscreenVert);
    shaders.load(kSsaoShader, { render::kFrameUbo },
                 { { "uSceneDepth", 0 } }, kFullscreenVert);
    mist.temporal.create(device);
    shaders.load(kMistShader,
                 { render::kFrameUbo, { "MistTemporalUbo", 3 } },
                 { { "uSceneDepth", 0 },
                   { "uShadowMap", 1 },
                   { "uCloudMap", 2 },
                   { "uMistMap", 8 },
                   { "uNoiseVolume", 9 },
                   { "uMistHistory", 10 },
                   { "uGiCascade0", 11 } },
                 kFullscreenVert);
    skyCloud.temporal.create(device);
    shaders.load(kSkyCloudsShader,
                 { render::kFrameUbo, { "CloudTemporalUbo", 3 } },
                 { { "uSceneDepth", 0 },
                   { "uCloudMap", 2 },
                   { "uNoiseVolume", 9 },
                   { "uCloudsHistory", 10 } },
                 kFullscreenVert);
    shaders.load(kCopyShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kBlurShader, {}, { { "uSource", 0 } }, kFullscreenVert);
    shaders.load(kLuminanceShader, {}, { { "uSceneColor", 0 } },
                 kFullscreenVert);
    shaders.load(kAdaptShader, { render::kFrameUbo },
                 { { "uLuminance", 0 }, { "uPrevExposure", 1 } },
                 kFullscreenVert);
    refreshPipelines(device, shaders);
}

void PostFx::destroy(rhi::Device& device) {
    (void)device; // every handle is rhi::Unique — the move-assign frees
    // them all through their device. The tuning knobs are the user's
    // settings, not resources: they ride over to the fresh object.
    PostFx fresh;
    fresh.froxelFog = froxelFog;
    fresh.froxelTemporalBlend = froxelTemporalBlend;
    fresh.froxelDustNoise = froxelDustNoise;
    fresh.mistTemporalBlend = mistTemporalBlend;
    fresh.cloudTemporalBlend = cloudTemporalBlend;
    *this = std::move(fresh);
}

void PostFx::refreshPipelines(rhi::Device& device, ShaderLibrary& shaders) {
    // Fullscreen passes: the fullscreen vertex stage + the pass's
    // fragment program, no vertex buffers.
    const auto fullscreen = [&](WatchedPipeline& pipeline, const char* name,
                                rhi::BlendMode blend) {
        pipeline.ensure(device, shaders, [&] {
            return device.createPipeline(
                { .shader = shaders.get(name), .blend = blend });
        });
    };
    fullscreen(prefilterPipeline, kPrefilterShader, rhi::BlendMode::Opaque);
    fullscreen(downPipeline, kDownShader, rhi::BlendMode::Opaque);
    fullscreen(upPipeline, kUpShader, rhi::BlendMode::Additive);
    fullscreen(godRayPipeline, kGodRaysShader, rhi::BlendMode::Opaque);
    fullscreen(volumetricPipeline, kVolumetricShader, rhi::BlendMode::Opaque);
    // The froxel programs are only loaded when the device can run them
    // (create(): compute + volume textures); the volumes stand for that
    // capability for the lifetime of the object, so this is an
    // availability gate, not a recipe input.
    if (froxelScatter[0].get().id != 0) {
        froxelInjectPipeline.ensure(device, shaders, [&] {
            return device.createComputePipeline(
                { shaders.get(kFroxelInjectShader) });
        });
        froxelIntegratePipeline.ensure(device, shaders, [&] {
            return device.createComputePipeline(
                { shaders.get(kFroxelIntegrateShader) });
        });
        fullscreen(froxelApplyPipeline, kFroxelApplyShader,
                   rhi::BlendMode::Opaque);
    }
    fullscreen(contactPipeline, kContactShader, rhi::BlendMode::Opaque);
    fullscreen(ssaoPipeline, kSsaoShader, rhi::BlendMode::Opaque);
    fullscreen(mistPipeline, kMistShader, rhi::BlendMode::Opaque);
    fullscreen(skyCloudPipeline, kSkyCloudsShader, rhi::BlendMode::Opaque);
    fullscreen(copyPipeline, kCopyShader, rhi::BlendMode::Opaque);
    fullscreen(blurPipeline, kBlurShader, rhi::BlendMode::Opaque);
    fullscreen(luminancePipeline, kLuminanceShader, rhi::BlendMode::Opaque);
    fullscreen(adaptPipeline, kAdaptShader, rhi::BlendMode::Opaque);
}

void PostFx::destroyTargets(rhi::Device& device) {
    (void)device; // every target frees through its wrapper
    for (u32 i = 0; i < 2; ++i) {
        adaptGroup[i].reset();
        adapt[i].reset();
    }
    luminanceGroup.reset();
    luminance.reset();
    contact.reset();
    ssao.reset();
    mist.resetTargets();
    skyCloud.resetTargets();
    skyCloudBlur.reset();
    volumetricGroup.reset();
    volumetric.reset();
    godRayGroup.reset();
    godRay.reset();
    prefilterGroup.reset();
    for (u32 i = 0; i < kBloomLevels; ++i) {
        downGroup[i].reset();
        upGroup[i].reset();
        bloom[i].reset();
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
        bloom[i].create(device, levelWidth, levelHeight,
                        rhi::TextureFormat::RGBA16F);
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
                             .texture = bloom[i - 1].tex,
                             .sampler = linearSampler } } }) };
    }
    for (u32 i = 0; i + 1 < kBloomLevels; ++i) {
        upGroup[i] = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = bloom[i + 1].tex,
                             .sampler = linearSampler } } }) };
    }

    // The half-res post targets differ only in format; their sampling
    // groups all tap the scene depth copy (god rays add the color copy).
    const u32 halfWidth = std::max(width / 2, 1u);
    const u32 halfHeight = std::max(height / 2, 1u);
    const auto makeDepthGroup = [&](rhi::UniqueBindGroup& group) {
        group = { device, device.createBindGroup(
                              { .entries = { { .binding = 0,
                                               .texture = sceneDepthCopy,
                                               .sampler = linearSampler } } }) };
    };

    godRay.create(device, halfWidth, halfHeight, rhi::TextureFormat::RGBA16F);
    // (godRayGroup is created below, after the sky-cloud targets exist —
    // the march composites the volumetric clouds itself.)

    volumetric.create(device, halfWidth, halfHeight,
                      rhi::TextureFormat::RGBA16F);
    makeDepthGroup(volumetricGroup);

    // Ground mist — same target family as the volumetric march; its own
    // texture so the tonemap composites mist and fog in the right order
    // and the A/B toggle stays independent.
    mist.createTargets(device, halfWidth, halfHeight, sceneDepthCopy,
                       linearSampler);

    // Volumetric sky clouds — the same target/history/temporal family.
    skyCloud.createTargets(device, halfWidth, halfHeight, sceneDepthCopy,
                           linearSampler);
    skyCloudBlur.create(device, halfWidth, halfHeight,
                        rhi::TextureFormat::RGBA16F, skyCloud.out.tex,
                        linearSampler);
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
    contact.raw.create(device, halfWidth, halfHeight,
                       rhi::TextureFormat::R16F);
    // Depth + scene color: the march reads the color ALPHA to exempt
    // grass from contact shadows on both sides (receiver early-out,
    // occluder rejection) — see contactshadow.frag. NEAREST sampling
    // on both (see nearestSampler in the header).
    contact.group = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneDepthCopy,
                         .sampler = nearestSampler.get() },
                       { .binding = 2,
                         .texture = sceneColorCopy,
                         .sampler = nearestSampler.get() } } }) };
    contact.blur.create(device, halfWidth, halfHeight,
                        rhi::TextureFormat::R16F, contact.raw.tex,
                        linearSampler);

    // SSAO — the contact pattern's sibling: half-res Alchemy taps over
    // the depth copy, then the shared 3x3 blur (IGN jitter filter); the
    // tonemap taps the BLURRED target.
    ssao.raw.create(device, halfWidth, halfHeight, rhi::TextureFormat::R16F);
    ssao.group = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneDepthCopy,
                         .sampler = nearestSampler.get() } } }) };
    ssao.blur.create(device, halfWidth, halfHeight, rhi::TextureFormat::R16F,
                     ssao.raw.tex, linearSampler);

    // Auto-exposure — fixed 64² log-luminance pyramid + the two
    // 1×1 adaptation targets (ping-pong; adapt.frag snaps when the prev
    // side reads 0, so fresh targets need no seeding).
    luminance.create(device, kLuminanceSize, kLuminanceSize,
                     rhi::TextureFormat::R16F, rhi::FilterMode::Linear,
                     7); // 64 -> 1
    luminanceGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColor,
                         .sampler = linearSampler } } }) };
    for (u32 i = 0; i < 2; ++i) {
        adapt[i].create(device, 1, 1, rhi::TextureFormat::R16F,
                        rhi::FilterMode::Nearest);
    }
    for (u32 i = 0; i < 2; ++i) {
        adaptGroup[i] = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = luminance.tex,
                             .sampler = linearSampler },
                           { .binding = 1,
                             .texture = adapt[1 - i].tex,
                             .sampler = linearSampler } } }) };
    }
    adaptSide = 0;
}

// --- Passes ----------------------------------------------------------------------------

void PostFx::renderContactShadows(rhi::CommandBuffer& cmd,
                                  rhi::BindGroupHandle frameBindGroup,
                                  rhi::BindGroupHandle shadowBindGroup) {
    if (!contact.raw.ready()) {
        return;
    }
    cmd.beginRenderPass({ .framebuffer = contact.raw.fb,
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
    cmd.setBindGroup(1, contact.group);
    cmd.draw(3);
    cmd.endRenderPass();

    // Speckle fix: filter the marching jitter (the tonemap taps the
    // blurred target).
    contact.blur.record(cmd, blurPipeline);
}

void PostFx::renderSsao(rhi::CommandBuffer& cmd,
                        rhi::BindGroupHandle frameBindGroup) {
    if (!ssao.raw.ready()) {
        return;
    }
    cmd.beginRenderPass({ .framebuffer = ssao.raw.fb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(ssaoPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, ssao.group);
    cmd.draw(3);
    cmd.endRenderPass();

    ssao.blur.record(cmd, blurPipeline);
}

void PostFx::clearSsao(rhi::CommandBuffer& cmd) {
    if (!ssao.blur.target.ready()) {
        return;
    }
    cmd.beginRenderPass({ .framebuffer = ssao.blur.target.fb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 1.0f, 1.0f, 1.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.endRenderPass();
}

void PostFx::clearContactShadows(rhi::CommandBuffer& cmd) {
    if (!contact.blur.target.ready()) {
        return;
    }
    // Toggle-off path: neutral white (there is no free FrameUbo slot for
    // a flag — the texture itself is the switch). The tonemap taps the
    // BLURRED target, so that is the one to neutralize.
    cmd.beginRenderPass({ .framebuffer = contact.blur.target.fb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 1.0f, 1.0f, 1.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.endRenderPass();
}

void PostFx::renderMedia(rhi::Device& device, rhi::CommandBuffer& cmd,
                         const FrameUniforms& frameData, MediaPass& pass,
                         rhi::PipelineHandle pipeline, f32 blend,
                         rhi::BindGroupHandle frameBindGroup,
                         std::initializer_list<SlotGroup> groups) {
    // Temporal state (the froxel invalidation rules): full-weight sample
    // on the first frame, after a camera jump, or with accumulation off.
    const Vec3 camera { frameData.cameraPos };
    const f32 alpha = pass.temporal.alpha(blend, camera);
    pass.temporal.upload(device, alpha, 0.0f, 0.0f);
    cmd.beginRenderPass({ .framebuffer = pass.out.fb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(pipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, pass.group);
    for (const SlotGroup& entry : groups) {
        if (entry.group.id != 0) {
            cmd.setBindGroup(entry.slot, entry.group);
        }
    }
    cmd.draw(3);
    cmd.endRenderPass();
    // Next frame's history = this frame's resolved target. A draw, not
    // copyTexture: vkCmdCopyImage costs ~1 ms in MoltenVK layout
    // transitions where this blit is ~0.1 ms.
    pass.history.record(cmd, copyPipeline);
    pass.temporal.commit(frameData.viewProj, camera, 0.0f, true);
}

void PostFx::renderMist(rhi::Device& device, rhi::CommandBuffer& cmd,
                        const FrameUniforms& frameData,
                        rhi::BindGroupHandle frameBindGroup,
                        rhi::BindGroupHandle shadowBindGroup,
                        rhi::BindGroupHandle giApplyGroup,
                        rhi::BindGroupHandle cloudMapGroup,
                        rhi::BindGroupHandle mistMapGroup,
                        rhi::BindGroupHandle noiseVolumeGroup) {
    if (!mist.out.ready() || mistMapGroup.id == 0) {
        return;
    }
    // Own slots for the cloud/mist maps: PostFx overwrites slot 3 with
    // the GI group, and on Vulkan an overwritten slot loses its bindings
    // (unlike GL's sticky texture units) — never rely on a group bound
    // by an earlier pass.
    renderMedia(device, cmd, frameData, mist, mistPipeline, mistTemporalBlend,
                frameBindGroup,
                { { 2, shadowBindGroup },
                  { 3, giApplyGroup },
                  { 4, noiseVolumeGroup },
                  { 5, cloudMapGroup },
                  { 6, mistMapGroup } });
}

void PostFx::clearMist(rhi::CommandBuffer& cmd) {
    if (!mist.out.ready()) {
        return;
    }
    // Toggle-off path: neutral (no inscatter, full transmittance) — the
    // texture itself is the switch, like the contact shadows.
    cmd.beginRenderPass({ .framebuffer = mist.out.fb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.endRenderPass();
    mist.temporal.valid = false; // stale mist must not bleed back in
}

void PostFx::renderSkyClouds(rhi::Device& device, rhi::CommandBuffer& cmd,
                             const FrameUniforms& frameData,
                             rhi::BindGroupHandle frameBindGroup,
                             rhi::BindGroupHandle noiseVolumeGroup,
                             rhi::BindGroupHandle cloudMapGroup) {
    if (!skyCloud.out.ready() || noiseVolumeGroup.id == 0) {
        return;
    }
    renderMedia(device, cmd, frameData, skyCloud, skyCloudPipeline,
                cloudTemporalBlend, frameBindGroup,
                { { 4, noiseVolumeGroup },
                  { 5, cloudMapGroup } }); // own slot (Vulkan rule)
    // Display-only 3x3 blur (the froxel lesson: the tonemap taps the
    // blurred target, the EMA history above stays sharp) — soaks up the
    // residual marching variance the LOD can't.
    skyCloudBlur.record(cmd, blurPipeline);
}

void PostFx::clearSkyClouds(rhi::CommandBuffer& cmd) {
    if (!skyCloud.out.ready()) {
        return;
    }
    // The tonemap taps the BLURRED target — that is the one to
    // neutralize (the sharp one too, for the next history).
    cmd.beginRenderPass({ .framebuffer = skyCloud.out.fb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.endRenderPass();
    cmd.beginRenderPass({ .framebuffer = skyCloudBlur.target.fb,
                          .loadOp = rhi::LoadOp::Clear,
                          .clearColor = { 0.0f, 0.0f, 0.0f, 1.0f },
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.endRenderPass();
    skyCloud.temporal.valid = false;
}

void PostFx::renderAutoExposure(rhi::Device& device, rhi::CommandBuffer& cmd,
                                rhi::BindGroupHandle frameBindGroup) {
    if (!luminance.ready()) {
        return;
    }
    // 1. Log-luminance of the HDR scene into the 64² base level.
    cmd.beginRenderPass({ .framebuffer = luminance.fb,
                          .loadOp = rhi::LoadOp::DontCare,
                          .depthLoadOp = rhi::LoadOp::DontCare });
    cmd.setPipeline(luminancePipeline);
    cmd.setBindGroup(0, luminanceGroup);
    cmd.draw(3);
    cmd.endRenderPass();

    // 2. Reduce to the 1×1 log-average (mip 6).
    device.generateMipmaps(luminance.tex);

    // 3. Adaptation micro-pass onto the other ping-pong side.
    adaptSide = 1 - adaptSide;
    cmd.beginRenderPass({ .framebuffer = adapt[adaptSide].fb,
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
        cmd.beginRenderPass({ .framebuffer = bloom[0].fb,
                              .loadOp = rhi::LoadOp::DontCare,
                              .depthLoadOp = rhi::LoadOp::DontCare });
        cmd.setPipeline(prefilterPipeline);
        cmd.setBindGroup(0, prefilterGroup);
        cmd.draw(3);
        cmd.endRenderPass();

        // Down the pyramid.
        for (u32 i = 1; i < kBloomLevels; ++i) {
            cmd.beginRenderPass({ .framebuffer = bloom[i].fb,
                                  .loadOp = rhi::LoadOp::DontCare,
                                  .depthLoadOp = rhi::LoadOp::DontCare });
            cmd.setPipeline(downPipeline);
            cmd.setBindGroup(0, downGroup[i]);
            cmd.draw(3);
            cmd.endRenderPass();
        }

        // Back up, additively widening the glow.
        for (i32 i = static_cast<i32>(kBloomLevels) - 2; i >= 0; --i) {
            cmd.beginRenderPass({ .framebuffer = bloom[i].fb,
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
        cmd.beginRenderPass({ .framebuffer = godRay.fb,
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
        f32 alpha = froxelTemporal.alpha(froxelTemporalBlend, camera);
        // Reach threshold at 5 m: the reprojection is reach-aware (the
        // history slice uses prevCamera.w), so the reach drifting with a
        // weather crossfade (fogStart-driven) must NOT invalidate every
        // frame — only the interior<->exterior jump should.
        if (std::abs(reach - froxelTemporal.prevReach) > 5.0f) {
            alpha = 1.0f;
        }
        froxelTemporal.upload(device, alpha,
                              glm::max(froxelTemporal.prevReach, 2.0f),
                              glm::clamp(froxelDustNoise, 0.0f, 1.0f));
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
        froxelTemporal.commit(frameData.viewProj, camera, reach,
                              frameData.time.z > 0.003f && reach > 0.0f);
        froxelSide = 1 - froxelSide;
        cmd.beginRenderPass({ .framebuffer = volumetric.fb,
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
        froxelTemporal.valid = false; // stale volumes must not be blended
        // The 2D march fallback: same output semantics, CSM-carved air.
        cmd.beginRenderPass({ .framebuffer = volumetric.fb,
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
