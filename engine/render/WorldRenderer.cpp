#include "engine/render/WorldRenderer.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/FrameContext.hpp"
#include "engine/assets/GltfMesh.hpp"
#include "engine/assets/MeshData.hpp" // render::MeshVertex / SkinnedVertex
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/render/MeshVertexLayout.hpp"
#include "engine/render/Projection.hpp"
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/render/MeshCache.hpp"
#include "engine/render/TextureCache.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/ui/UiSystem.hpp"
#include "engine/render/FrameComposer.hpp"

namespace render {

namespace {

constexpr const char* kTonemapShader = "tonemap";

// std140 ModelUbo mirror (model + tint + info), shared by the mesh and
// skinned draws.
struct ModelUniforms {
    Mat4 model { 1.0f };
    Vec4 tint { 1.0f };
    Vec4 info { 0.0f }; // x = emissive
};

// std140 LightsUbo mirror (binding 5 — mirrors locallights.glsl).
struct LightsUniforms {
    Vec4 count { 0.0f };
    Vec4 positionRadius[WorldRenderer::kMaxLights] {};
    Vec4 colorIntensity[WorldRenderer::kMaxLights] {};
    // xyz = spot direction, w = cos(half angle); w = -2 marks a point
    // light, w = -3 a WINDOW projector (xyz = the window's into-room
    // normal — docs/RENDERING.md §3).
    Vec4 directionAngle[WorldRenderer::kMaxLights] {};
    // Window projector half extents (xy); zw free. APPEND-only UBO.
    Vec4 windowInfo[WorldRenderer::kMaxLights] {};
};

} // namespace

// H3 (docs/RENDERING.md): 0 below the worldspace's buried threshold,
// 1 above, 4 m fade band — sun-linked lights and the daylight coupling
// fade out per POSITION. Threshold -1e9 = the rule is off (always 1).
static f32 aboveBuried(f32 y, f32 threshold) {
    return glm::smoothstep(threshold - 2.0f, threshold + 2.0f, y);
}

void WorldRenderer::create(rhi::Device& device, core::JobSystem& jobs,
                               const RendererConfig& config) {
    cfg = config;
    if (!cfg.postFx) {
        cfg.froxels = false; // froxel fog lives in the postFx chain
    }
    if (!cfg.froxels) {
        postFx.froxelFog = false;
    }
    frameUbo = { device, device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                     .size = sizeof(render::FrameUniforms),
                                     .dynamic = true },
                                   nullptr) };
    // Local lights ride binding 5 of the SAME group — shaders that
    // don't declare the block simply ignore it.
    lightsUbo = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          // Sized from the STRUCT, not a hand-counted formula — the
          // formula missed the appended windowInfo array and the update
          // silently failed (range > size), freezing every light.
          .size = sizeof(LightsUniforms),
          .dynamic = true },
        nullptr) };
    // The cluster SSBO (docs/RENDERING.md §5) rides binding 4 of the frame
    // group so every pass sees the lists — created BEFORE the group.
    if (device.caps().computeShaders) {
        lightClusters.createBuffer(device);
    }
    rhi::BindGroupDesc frameGroupDesc {
        .entries = { { .binding = 0, .buffer = frameUbo },
                     { .binding = 5, .buffer = lightsUbo } }
    };
    if (lightClusters.clusterBuffer().id != 0) {
        frameGroupDesc.entries.push_back(
            { .binding = 4,
              .buffer = lightClusters.clusterBuffer(),
              .storage = true });
    }
    frameBindGroup = { device, device.createBindGroup(frameGroupDesc) };

    // Per-instance ShaderLibrary (multi-instance audit, §7 R3): each
    // renderer owns its programs and hot-reload generations. Sharing one
    // library would save recompiles but couple instance lifetimes; the
    // persisted pipeline cache already amortizes the compile cost.
    shaders = std::make_unique<render::ShaderLibrary>(device);
    if (cfg.terrain) {
        terrain.create(device, *shaders, jobs,
                       { .albedo = cfg.terrainAlbedoPath,
                         .normal = cfg.terrainNormalPath,
                         .orm = cfg.terrainOrmPath,
                         .height = cfg.terrainHeightPath });
        terrainLightMap.create(device, jobs);
        terrainShadeMap.create(device, jobs);
        farTerrain.create(device, *shaders, jobs);
        if (cfg.postFx) {
            mistMap.create(device, jobs);
            noiseVolume.create(device, *shaders); // no-op without caps
        }
    }
    if (cfg.occlusion) {
        occlusion.create(jobs);
    }
    if (cfg.gi) {
        radianceCascades.create(device, *shaders, jobs);
    }
    if (cfg.grass) {
        grass.create(device, *shaders, jobs);
    }
    if (cfg.vegetation) {
        vegetation.create(device, *shaders, jobs,
                          terrain.params.seed);
    }

    // (The mesh/texture residency caches stay SCENE-owned — the editor and
    // streaming share them; the view hands them in per frame.)
    const u32 white = 0xFFFFFFFF;
    whiteTexture = { device, device.createTexture(
        { .width = 1, .height = 1, .format = rhi::TextureFormat::SRGBA8 },
        &white) };
    meshSampler = { device, device.createSampler({}) };
    shaders->load("mesh",
                  { { "FrameUbo", 0 }, { "ModelUbo", 1 },
                    { "LightsUbo", 5 } },
                  { { "uAlbedo", 0 } });
    buildMeshPipeline(device);
    // The depth-only caster variants (sun cascades).
    shaders->load("shadow_mesh",
                  { { "ShadowUbo", 1 }, { "CasterModelUbo", 4 } });
    shaders->load("shadow_skinned",
                  { { "ShadowUbo", 1 }, { "CasterModelUbo", 4 } });
    buildCasterPipelines(device);
    // Placed water surfaces.
    shaders->load("watervolume",
                  { { "FrameUbo", 0 }, { "WaterVolumeUbo", 1 } });
    // (No volumetric cumulonimbus pass — cost over look. The cloud MAP
    // keeps the sky alive; stormFront still drives rain/wetness via
    // stormInfo.y.)

    // Rain — procedural streaks (no buffers) + the top-down
    // occlusion depth so roofs keep the drops out.
    if (cfg.sky) {
        shaders->load("rain", { { "FrameUbo", 0 } },
                      { { "uRainOcclusion", 9 } });
        rainOcclusionTex = { device, device.createTexture(
            { .width = 512,
              .height = 512,
              .format = rhi::TextureFormat::Depth32F,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr) };
        rainSampler = { device, device.createSampler({}) };
        rainOcclusionFb = { device, device.createFramebuffer(
            { .depthAttachment = { .texture = rainOcclusionTex } }) };
        rainOcclusionUbo = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Uniform,
              .size = sizeof(Mat4),
              .dynamic = true },
            nullptr) };
        rainCasterGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 1,
                             .buffer = rainOcclusionUbo } } }) };
        rainReceiverGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 9,
                             .texture = rainOcclusionTex,
                             .sampler = rainSampler } } }) };
    }

    // The key-shadow atlas (2048² = 2x2 perspective tiles, §5 B6).
    keyShadowTex = { device, device.createTexture(
        { .width = 2048,
          .height = 2048,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr) };
    keyShadowSampler = { device, device.createSampler(
        { .minFilter = rhi::FilterMode::Linear,
          .magFilter = rhi::FilterMode::Linear,
          .compare = rhi::CompareFunc::LessEqual }) };
    keyShadowFb = { device, device.createFramebuffer(
        { .depthAttachment = { .texture = keyShadowTex } }) };
    for (u32 slot = 0; slot < kKeyShadowSlots; ++slot) {
        keyShadowUbos[slot] =
            { device, device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                  .size = sizeof(Mat4),
                                  .dynamic = true },
                                nullptr) };
        keyShadowCasterGroups[slot] = { device, device.createBindGroup(
            { .entries = { { .binding = 1,
                             .buffer = keyShadowUbos[slot] } } }) };
    }
    keyShadowReceiverGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 6,
                         .texture = keyShadowTex,
                         .sampler = keyShadowSampler } } }) };

    shaders->load("skinned",
                  { { "FrameUbo", 0 }, { "ModelUbo", 1 },
                    { "LightsUbo", 5 } },
                  { { "uAlbedo", 0 } });
    // Swap one procedural rock variant for an authored CC0 glTF
    // rock (moon_rock_02, Poly Haven). Missing file = procedural fallback.
    if (auto rock = cfg.vegetation
                        ? assets::loadGltfMesh(platform::executableDir() /
                                               "data" / "base" / "models" /
                                               "rock_cc0.gltf")
                        : std::nullopt) {
        assets::normalizeMesh(*rock, 2.2f);
        for (render::MeshVertex& vertex : rock->vertices) {
            vertex.uv = { 0.0f, 0.0f }; // rigid: no canopy sway
            // The scan's albedo lives in a texture we don't sample; tint
            // the white base color down to the procedural rocks' gray.
            vertex.color *= Vec3 { 0.125f, 0.120f, 0.115f };
        }
        vegetation.overrideVariantMesh(
            device, render::VegetationSystem::kFirstRock, std::move(*rock));
        LOG_INFO("glTF rock loaded as rock variant 0");
    }
    if (cfg.sky) {
        sky.create(device, *shaders);
    }
    if (device.caps().offscreenTargets && device.caps().textureArrays) {
        shadows.create(device);
    }
    if (device.caps().copyTexture) {
        fx.create(device, *shaders); // the particle pass
        // The depth copy's sampler — Hi-Z and postFx read it too, so it
        // lives outside the water gate.
        depthSampler = { device, device.createSampler(
            { .minFilter = rhi::FilterMode::Nearest,
              .magFilter = rhi::FilterMode::Nearest }) };
    }
    if (cfg.water && device.caps().copyTexture) {
        water.create(device, *shaders, jobs);
        reflectionUbo =
            { device, device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                  .size = sizeof(render::FrameUniforms),
                                  .dynamic = true },
                                nullptr) };
        reflectionBindGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 0, .buffer = reflectionUbo } } }) };
    }

    if (device.caps().offscreenTargets) {
        blitSampler = { device, device.createSampler({}) }; // linear, clamp — identity
        shaders->load(kTonemapShader, { { "FrameUbo", 0 } },
                      { { "uSceneColor", 0 },
                        { "uBloom", 1 },
                        { "uGodRays", 2 },
                        { "uVolumetric", 3 },
                        { "uMist", 4 },       // ground mist
                        { "uExposure", 5 },   // adaptation tap
                        { "uContact", 6 },    // contact shadows
                        { "uSkyClouds", 7 },  // volumetric clouds
                        { "uSceneDepth", 8 },   // bilateral weights
                        { "uSsao", 9 } });
        shaders->load("ssdm_flow", { { "FrameUbo", 0 } },
                      { { "uSceneColor", 0 }, { "uSceneDepth", 1 } },
                      "fullscreen");
        shaders->load("ssdm_bounds0", { { "FrameUbo", 0 } },
                      { { "uFlow", 0 } }, "fullscreen");
        shaders->load("ssdm_bounds_down", { { "FrameUbo", 0 } },
                      { { "uPrev", 0 } }, "fullscreen");
        shaders->load("ssdm_resolve_half", { { "FrameUbo", 0 } },
                      { { "uSceneColor", 0 },
                        { "uSceneDepth", 1 },
                        { "uFlow", 2 },
                        { "uBounds0", 3 },
                        { "uBounds1", 4 },
                        { "uBounds2", 5 },
                        { "uBounds3", 6 },
                        { "uBounds4", 7 } },
                      "fullscreen");
        shaders->load("ssdm_upsample", { { "FrameUbo", 0 } },
                      { { "uHalf", 0 }, { "uSceneColor", 1 } },
                      "fullscreen");
        shaders->load("ssdm_resolve", { { "FrameUbo", 0 } },
                      { { "uSceneColor", 0 },
                        { "uSceneDepth", 1 },
                        { "uFlow", 2 },
                        { "uBounds0", 3 },
                        { "uBounds1", 4 },
                        { "uBounds2", 5 },
                        { "uBounds3", 6 },
                        { "uBounds4", 7 } },
                      "fullscreen");
        rebuildBlitPipeline(device);
    }
    if (cfg.postFx && device.caps().offscreenTargets &&
        device.caps().hdrFormats && device.caps().copyTexture) {
        postFx.create(device, *shaders);
    }
    if (cfg.occlusion && device.caps().computeShaders &&
        device.caps().copyTexture && device.caps().offscreenTargets) {
        gpuOcclusion.create(device, *shaders);
    }
    if (lightClusters.clusterBuffer().id != 0) {
        lightClusters.createPipeline(device, *shaders);
    }
}

void WorldRenderer::destroy(rhi::Device& device) {
    // Every handle is an rhi::Unique — clearing/resetting frees it
    // through its device; there is no manual destroy mirror to keep in
    // sync. Must run while the device is alive (wrapper contract).
    gpuProbe.shutdown(device); // abandon in-flight timestamps
    destroyOffscreenTarget(device);
    blitPipeline.reset();
    blitSampler.reset();
    // Mesh path: per-entry draw state (the residency caches are
    // scene-owned; their dtors free what they own).
    meshDraws.clear();
    meshPipeline.reset();
    meshCasterPipeline.reset();
    skinnedCasterPipeline.reset();
    waterQuads.clear();
    waterVolumePipeline.reset();
    rainPipeline.reset();
    rainReceiverGroup.reset();
    rainCasterGroup.reset();
    rainOcclusionUbo.reset();
    rainOcclusionFb.reset();
    rainSampler.reset();
    rainOcclusionTex.reset();
    keyShadowReceiverGroup.reset();
    for (u32 slot = 0; slot < kKeyShadowSlots; ++slot) {
        keyShadowCasterGroups[slot].reset();
        keyShadowUbos[slot].reset();
    }
    keyShadowFb.reset();
    keyShadowSampler.reset();
    keyShadowTex.reset();
    skinnedDraws.clear();
    skinnedPipeline.reset();
    skinnedShaderGeneration = 0;
    meshSampler.reset();
    whiteTexture.reset();
    lightClusters.destroy(device);
    gpuOcclusion.destroy(device);
    terrainLightMap.destroy(device);
    terrainShadeMap.destroy(device);
    farTerrain.destroy(device);
    mistMap.destroy(device);
    noiseVolume.destroy(device);
    radianceCascades.destroy(device);
    postFx.destroy(device);
    water.destroy(device);
    fx.destroy(device);
    reflectionBindGroup.reset();
    reflectionUbo.reset();
    depthSampler.reset();
    shadows.destroy(device);
    sky.destroy(device);
    vegetation.destroy(device);
    grass.destroy(device);
    terrain.destroy(device);
    shaders.reset(); // destroys the library's shader programs
    frameBindGroup.reset();
    lightsUbo.reset();
    frameUbo.reset();
    sculptDirtyChunks.clear();
    sculptScatterChunks.clear();
}

void WorldRenderer::ensureOffscreenTarget(rhi::Device& device, u32 width,
                                           u32 height) {
    if (offscreenFb.id() != 0 && offscreenWidth == width &&
        offscreenHeight == height &&
        appliedReflectionScale == reflectionScaleUi &&
        appliedSsdmMode == ssdmModeUi) {
        return;
    }
    appliedSsdmMode = ssdmModeUi;
    destroyOffscreenTarget(device);
    // HDR scene target: the sky/sun palette is linear HDR (sun > 1); the
    // tonemap pass compresses to display range.
    offscreenColor = { device, device.createTexture(
        { .width = width,
          .height = height,
          .format = device.caps().hdrFormats ? rhi::TextureFormat::RGBA16F
                                             : rhi::TextureFormat::RGBA8,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr) };
    offscreenDepth = { device, device.createTexture(
        { .width = width,
          .height = height,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_RenderAttachment },
        nullptr) };
    offscreenFb = { device, device.createFramebuffer(
        { .colorAttachments = { { .texture = offscreenColor } },
          .depthAttachment = { .texture = offscreenDepth } }) };
    if (device.caps().copyTexture) {
        sceneColorCopy = { device, device.createTexture(
            { .width = width,
              .height = height,
              .format = device.caps().hdrFormats ? rhi::TextureFormat::RGBA16F
                                                 : rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            nullptr) };
        sceneDepthCopy = { device, device.createTexture(
            { .width = width,
              .height = height,
              .format = rhi::TextureFormat::Depth32F,
              .usage = rhi::TextureUsage_Sampled },
            nullptr) };
        if (cfg.water) {
            // The reflection resolution is a knob (docs/RENDERING.md) —
            // 0.5 = half res, 0.25 = quarter res (blurrier mirror).
            const u32 reflectionWidth = glm::max(
                static_cast<u32>(static_cast<f32>(width) *
                                 reflectionScaleUi),
                1u);
            const u32 reflectionHeight = glm::max(
                static_cast<u32>(static_cast<f32>(height) *
                                 reflectionScaleUi),
                1u);
            appliedReflectionScale = reflectionScaleUi;
            reflectionColor = { device, device.createTexture(
                { .width = reflectionWidth,
                  .height = reflectionHeight,
                  .format = device.caps().hdrFormats
                                ? rhi::TextureFormat::RGBA16F
                                : rhi::TextureFormat::RGBA8,
                  .filter = rhi::FilterMode::Linear,
                  .usage = rhi::TextureUsage_Sampled |
                           rhi::TextureUsage_RenderAttachment },
                nullptr) };
            reflectionDepth = { device, device.createTexture(
                { .width = reflectionWidth,
                  .height = reflectionHeight,
                  .format = rhi::TextureFormat::Depth32F,
                  .usage = rhi::TextureUsage_RenderAttachment },
                nullptr) };
            reflectionFb = { device, device.createFramebuffer(
                { .colorAttachments = { { .texture = reflectionColor } },
                  .depthAttachment = { .texture = reflectionDepth } }) };
        }
    }

    if (cfg.postFx && device.caps().offscreenTargets &&
        device.caps().hdrFormats && device.caps().copyTexture) {
        postFx.resize(device, width, height, offscreenColor, sceneColorCopy,
                      sceneDepthCopy);
    }

    // After postFx.resize: the water shader reprojects the sky-cloud
    // display buffer into the mirror, so the group must reference the
    // freshly (re)created texture.
    if (cfg.water && reflectionColor.id() != 0) {
        rhi::BindGroupDesc desc { .entries = {
            { .binding = 0,
              .texture = sceneColorCopy,
              .sampler = blitSampler },
            { .binding = 1,
              .texture = sceneDepthCopy,
              .sampler = depthSampler },
            { .binding = 2,
              .texture = reflectionColor,
              .sampler = blitSampler } } };
        if (postFx.skyCloudsTexture().id != 0) {
            desc.entries.push_back({ .binding = 4,
                                     .texture = postFx.skyCloudsTexture(),
                                     .sampler = blitSampler });
        }
        waterSceneBindGroup = { device, device.createBindGroup(desc) };
    }
    // SSDM chain targets: flow + the halving bounds pyramid, at the
    // mode's resolution (full / half of the scene target / a token 4x4
    // when off — the memory is not free at Retina sizes).
    const u32 chainW = ssdmModeUi == 2   ? width
                       : ssdmModeUi == 1 ? glm::max(width / 2, 8u)
                                         : 4u;
    const u32 chainH = ssdmModeUi == 2   ? height
                       : ssdmModeUi == 1 ? glm::max(height / 2, 8u)
                                         : 4u;
    ssdmFlowTex = { device, device.createTexture(
        { .width = chainW,
          .height = chainH,
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Nearest,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr) };
    ssdmFlowFb = { device, device.createFramebuffer(
        { .colorAttachments = { { .texture = ssdmFlowTex.get() } } }) };
    ssdmFlowGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColorCopy,
                         .sampler = blitSampler },
                       { .binding = 1,
                         .texture = sceneDepthCopy,
                         .sampler = blitSampler } } }) };
    u32 levelW = chainW;
    u32 levelH = chainH;
    for (u32 i = 0; i < kSsdmLevels; ++i) {
        ssdmBoundsTex[i] = { device, device.createTexture(
            { .width = levelW,
              .height = levelH,
              .format = rhi::TextureFormat::RGBA16F,
              .filter = rhi::FilterMode::Nearest,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr) };
        ssdmBoundsFb[i] = { device, device.createFramebuffer(
            { .colorAttachments = { { .texture =
                                          ssdmBoundsTex[i].get() } } }) };
        ssdmBoundsGroup[i] = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = i == 0 ? ssdmFlowTex.get()
                                               : ssdmBoundsTex[i - 1]
                                                     .get(),
                             .sampler = blitSampler } } }) };
        levelW = std::max(levelW / 2, 1u);
        levelH = std::max(levelH / 2, 1u);
    }
    ssdmResolveGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = sceneColorCopy,
                         .sampler = blitSampler },
                       { .binding = 1,
                         .texture = sceneDepthCopy,
                         .sampler = blitSampler },
                       { .binding = 2,
                         .texture = ssdmFlowTex.get(),
                         .sampler = blitSampler },
                       { .binding = 3,
                         .texture = ssdmBoundsTex[0].get(),
                         .sampler = blitSampler },
                       { .binding = 4,
                         .texture = ssdmBoundsTex[1].get(),
                         .sampler = blitSampler },
                       { .binding = 5,
                         .texture = ssdmBoundsTex[2].get(),
                         .sampler = blitSampler },
                       { .binding = 6,
                         .texture = ssdmBoundsTex[3].get(),
                         .sampler = blitSampler },
                       { .binding = 7,
                         .texture = ssdmBoundsTex[4].get(),
                         .sampler = blitSampler } } }) };
    ssdmHalfTex = { device, device.createTexture(
        { .width = chainW,
          .height = chainH,
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr) };
    ssdmHalfFb = { device, device.createFramebuffer(
        { .colorAttachments = { { .texture = ssdmHalfTex.get() } } }) };
    ssdmUpsampleGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0,
                         .texture = ssdmHalfTex.get(),
                         .sampler = blitSampler },
                       { .binding = 1,
                         .texture = sceneColorCopy,
                         .sampler = blitSampler } } }) };
    if (shaders) {
        const auto pipe = [&](rhi::UniquePipeline& p, const char* name) {
            p = { device,
                  device.createPipeline(
                      { .shader = shaders->get(name),
                        .blend = rhi::BlendMode::Opaque }) };
        };
        pipe(ssdmFlowPipeline, "ssdm_flow");
        pipe(ssdmBounds0Pipeline, "ssdm_bounds0");
        pipe(ssdmDownPipeline, "ssdm_bounds_down");
        pipe(ssdmResolvePipeline, "ssdm_resolve");
        pipe(ssdmResolveHalfPipeline, "ssdm_resolve_half");
        pipe(ssdmUpsamplePipeline, "ssdm_upsample");
    }
    // Tonemap inputs: scene + bloom + god rays (black 1x1 fallbacks are not
    // needed on the 4.6 path — postFx is always ready when we get here).
    // One group per adaptation ping-pong side (binding 5).
    for (u32 side = 0; side < 2; ++side) {
        blitBindGroups[side] = { device, device.createBindGroup(
            { .entries =
                  postFx.ready()
                      ? vector<rhi::BindGroupEntry> {
                            { .binding = 0,
                              .texture = offscreenColor,
                              .sampler = blitSampler },
                            { .binding = 1,
                              .texture = postFx.bloomTexture(),
                              .sampler = blitSampler },
                            { .binding = 2,
                              .texture = postFx.godRayTexture(),
                              .sampler = blitSampler },
                            { .binding = 3,
                              .texture = postFx.volumetricTexture(),
                              .sampler = blitSampler },
                            { .binding = 4,
                              .texture = postFx.mistTexture(),
                              .sampler = blitSampler },
                            { .binding = 5,
                              .texture = postFx.exposureTexture(side),
                              .sampler = blitSampler },
                            { .binding = 6,
                              .texture = postFx.contactTexture(),
                              .sampler = blitSampler },
                            { .binding = 7,
                              .texture = postFx.skyCloudsTexture(),
                              .sampler = blitSampler },
                            { .binding = 8,
                              .texture = sceneDepthCopy,
                              .sampler = blitSampler },
                            { .binding = 9,
                              .texture = postFx.ssaoTexture(),
                              .sampler = blitSampler } }
                      : vector<rhi::BindGroupEntry> {
                            { .binding = 0,
                              .texture = offscreenColor,
                              .sampler = blitSampler } } }) };
    }
    offscreenWidth = width;
    offscreenHeight = height;
    LOG_INFO("Offscreen scene target: {}x{}", width, height);
}

void WorldRenderer::destroyOffscreenTarget(rhi::Device& device) {
    (void)device; // the Unique wrappers free through their device
    if (offscreenFb.id() == 0) {
        return;
    }
    waterSceneBindGroup.reset();
    reflectionFb.reset();
    reflectionDepth.reset();
    reflectionColor.reset();
    sceneDepthCopy.reset();
    sceneColorCopy.reset();
    blitBindGroups[0].reset();
    blitBindGroups[1].reset();
    offscreenFb.reset();
    offscreenDepth.reset();
    offscreenColor.reset();
    offscreenWidth = 0;
    offscreenHeight = 0;
}

void WorldRenderer::rebuildBlitPipeline(rhi::Device& device) {
    // The assignment frees the previous pipeline through the wrapper.
    blitPipeline = { device, device.createPipeline(
                                 { .shader = shaders->get(kTonemapShader) }) };
    blitShaderGeneration = shaders->generation(kTonemapShader);
}

void WorldRenderer::drawSceneMeshes(engine::FrameContext& frame,
                                        const render::RenderSnapshot& snapshot,
                                        const RenderView& view) {
    if (snapshot.meshes.empty()) {
        return;
    }
    if (shaders->generation("mesh") != meshShaderGeneration) {
        buildMeshPipeline(frame.device);
    }
    if (meshDraws.size() < snapshot.meshes.size()) {
        meshDraws.resize(snapshot.meshes.size());
    }
    frame.cmd.setPipeline(meshPipeline);
    frame.cmd.setBindGroup(0, frameBindGroup);
    for (u32 i = 0; i < snapshot.meshes.size(); ++i) {
        const render::RenderSnapshot::MeshInstance& instance =
            snapshot.meshes[i];
        const render::MeshCache::Gpu& mesh =
            view.meshCache->resolve(instance.model);

        // Material fields resolved at extract; only the TEXTURE
        // residency lookup stays draw-side (it is a GPU cache).
        ModelUniforms uniforms;
        uniforms.model = instance.transform;
        uniforms.tint = instance.tint;
        uniforms.info.x = instance.emissive;
        rhi::TextureHandle albedo = whiteTexture;
        if (instance.albedoTexture.isValid()) {
            const rhi::TextureHandle resolved =
                view.materialTextures->resolve(instance.albedoTexture);
            if (resolved.id != 0) {
                albedo = resolved;
            }
        }

        MeshDraw& draw = meshDraws[i];
        if (draw.ubo.id() == 0) {
            draw.ubo = { frame.device, frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  .size = sizeof(ModelUniforms),
                  .dynamic = true },
                nullptr) };
        }
        frame.device.updateBuffer(draw.ubo, &uniforms, sizeof(uniforms), 0);
        if (draw.group.id() == 0 || draw.boundTexture.id != albedo.id ||
            draw.material != instance.material) {
            // The assignment frees the previous group.
            draw.group = { frame.device, frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = draw.ubo },
                               { .binding = 0,
                                 .texture = albedo,
                                 .sampler = meshSampler } } }) };
            draw.boundTexture = albedo;
            draw.material = instance.material;
        }
        frame.cmd.setBindGroup(1, draw.group);
        frame.cmd.setVertexBuffer(0, mesh.vertices);
        frame.cmd.setIndexBuffer(mesh.indices, rhi::IndexFormat::U32);
        frame.cmd.drawIndexed(mesh.indexCount);
    }
}

// --- First-person player -----------------------------------------------------

void WorldRenderer::drawSkinned(engine::FrameContext& frame,
                                    const render::RenderSnapshot& snapshot) {
    if (snapshot.skinned.empty() && skinnedDraws.empty()) {
        return;
    }
    if (shaders->generation("skinned") != skinnedShaderGeneration) {
        buildSkinnedPipeline(frame.device);
    }
    for (SkinnedDraw& draw : skinnedDraws) {
        draw.seen = false;
    }
    bool any = false;
    for (const render::RenderSnapshot::SkinnedInstance& instance :
         snapshot.skinned) {
        SkinnedDraw* slot = nullptr;
        for (SkinnedDraw& draw : skinnedDraws) {
            if (draw.entityId == instance.entityId) {
                slot = &draw;
                break;
            }
        }
        if (!slot) {
            skinnedDraws.push_back({ instance.entityId });
            slot = &skinnedDraws.back();
        }
        slot->seen = true;
        if (slot->paletteSsbo.id() == 0) {
            slot->paletteSsbo = { frame.device, frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Storage,
                  .size = instance.palette.size() * sizeof(Mat4),
                  .dynamic = true },
                instance.palette.data()) };
            slot->modelUbo = { frame.device, frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  // std140 ModelUbo: mat4 model + vec4 tint + vec4 info.
                  .size = sizeof(Mat4) + 2 * sizeof(Vec4),
                  .dynamic = true },
                nullptr) };
            slot->group = { frame.device, frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = slot->modelUbo },
                               { .binding = 0,
                                 .texture = whiteTexture,
                                 .sampler = meshSampler },
                               { .binding = 2,
                                 .buffer = slot->paletteSsbo,
                                 .storage = true } } }) };
        }
        ModelUniforms uniforms;
        uniforms.model = instance.transform;
        uniforms.tint = instance.tint;
        frame.device.updateBuffer(slot->modelUbo, &uniforms,
                                  sizeof(uniforms), 0);
        frame.device.updateBuffer(slot->paletteSsbo,
                                  instance.palette.data(),
                                  instance.palette.size() * sizeof(Mat4), 0);
        if (!any) {
            frame.cmd.setPipeline(skinnedPipeline);
            frame.cmd.setBindGroup(0, frameBindGroup);
            any = true;
        }
        frame.cmd.setBindGroup(1, slot->group);
        frame.cmd.setVertexBuffer(0, instance.vertices);
        frame.cmd.setIndexBuffer(instance.indices, rhi::IndexFormat::U32);
        frame.cmd.drawIndexed(instance.indexCount);
    }
    // Sweep draws whose NPC was pruned (cell unload / death cleanup).
    for (auto it = skinnedDraws.begin(); it != skinnedDraws.end();) {
        if (!it->seen) {
            it = skinnedDraws.erase(it); // Unique members self-free
        } else {
            ++it;
        }
    }
}

void WorldRenderer::buildSkinnedPipeline(rhi::Device& device) {
    skinnedPipeline = { device, device.createPipeline(
        { .shader = shaders->get("skinned"),
          .vertexBuffers =
              { { .stride = sizeof(render::SkinnedVertex),
                  .attributes =
                      { { .location = 0,
                          .format = rhi::VertexFormat::F32x3,
                          .offset =
                              offsetof(render::SkinnedVertex, position) },
                        { .location = 1,
                          .format = rhi::VertexFormat::F32x3,
                          .offset = offsetof(render::SkinnedVertex, normal) },
                        { .location = 2,
                          .format = rhi::VertexFormat::F32x2,
                          .offset = offsetof(render::SkinnedVertex, uv) },
                        { .location = 3,
                          .format = rhi::VertexFormat::F32x3,
                          .offset = offsetof(render::SkinnedVertex, color) },
                        { .location = 4,
                          .format = rhi::VertexFormat::F32x4,
                          .offset = offsetof(render::SkinnedVertex, joints) },
                        { .location = 5,
                          .format = rhi::VertexFormat::F32x4,
                          .offset =
                              offsetof(render::SkinnedVertex, weights) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::Back }) };
    skinnedShaderGeneration = shaders->generation("skinned");
}

// Bundle the streaming fixups' systems for StreamingController this frame —
// references into the scene plus the focus / fade / mode scalars. Rebuilt each

f32 WorldRenderer::effectiveWaterSurfaceY(
    const render::RenderSnapshot& snapshot, const RenderView& view) const {
    // The water surface the CAMERA sits under, if any — sea
    // level outdoors, a volume's top when inside one (any worldspace),
    // "dry" otherwise. Feeds the tonemap submersion.
    f32 surface = view.interiorMode ? -1.0e6f : terrain.params.seaLevel;
    const Vec3 eye = view.camera.position;
    // Local lakes/rivers count too: without this, diving under an
    // altitude lake showed no submersion tint at all.
    if (!view.interiorMode) {
        if (const render::WaterBodies* bodies = water.currentBodies()) {
            const auto local = render::terrain::waterSurfaceAt(
                *bodies, eye.x, eye.z, eye.y);
            if (local) {
                surface = glm::max(surface, *local);
            }
        }
    }
    for (const render::WaterVolumeInstance& volume : snapshot.waterVolumes) {
        const Vec3 d = eye - volume.position;
        if (std::abs(d.x) <= volume.halfExtents.x &&
            std::abs(d.z) <= volume.halfExtents.z && d.y >= 0.0f &&
            d.y <= volume.halfExtents.y * 2.0f) {
            surface = glm::max(surface,
                               volume.position.y + volume.halfExtents.y * 2.0f);
        }
    }
    return surface;
}

void WorldRenderer::drawWaterVolumes(
    engine::FrameContext& frame, const render::RenderSnapshot& snapshot) {
    if (shaders->generation("watervolume") != waterVolumeShaderGeneration ||
        waterVolumePipeline.id() == 0) {
        waterVolumePipeline = { frame.device, frame.device.createPipeline(
            { .shader = shaders->get("watervolume"),
              .vertexBuffers =
                  { { .stride = 3 * sizeof(f32),
                      .attributes = { { .location = 0,
                                        .format = rhi::VertexFormat::F32x3,
                                        .offset = 0 } } } },
              .blend = rhi::BlendMode::Alpha,
              .depth = { .testEnable = true,
                         .writeEnable = false,
                         .compare = rhi::CompareFunc::Greater }, // reversed-Z
              .cull = rhi::CullMode::None }) };
        waterVolumeShaderGeneration = shaders->generation("watervolume");
    }
    for (WaterQuad& quad : waterQuads) {
        quad.seen = false;
    }
    bool any = false;
    for (const render::WaterVolumeInstance& volume : snapshot.waterVolumes) {
        WaterQuad* slot = nullptr;
        for (WaterQuad& quad : waterQuads) {
            if (quad.entityId == volume.entityId) {
                slot = &quad;
                break;
            }
        }
        if (!slot) {
            waterQuads.push_back({ volume.entityId });
            slot = &waterQuads.back();
        }
        slot->seen = true;
        if (slot->vertices.id() == 0) {
            // The box TOP face, two triangles in world space.
            const Vec3 c = volume.position +
                           Vec3 { 0.0f, volume.halfExtents.y * 2.0f, 0.0f };
            const f32 hx = volume.halfExtents.x;
            const f32 hz = volume.halfExtents.z;
            const f32 verts[18] = {
                c.x - hx, c.y, c.z - hz, c.x + hx, c.y, c.z - hz,
                c.x + hx, c.y, c.z + hz, c.x - hx, c.y, c.z - hz,
                c.x + hx, c.y, c.z + hz, c.x - hx, c.y, c.z + hz,
            };
            slot->vertices = { frame.device, frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex, .size = sizeof(verts) },
                verts) };
            slot->ubo = { frame.device, frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  .size = sizeof(Vec4),
                  .dynamic = true },
                nullptr) };
            slot->group = { frame.device, frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = slot->ubo } } }) };
            const Vec4 tint { volume.tint, volume.chop };
            frame.device.updateBuffer(slot->ubo, &tint, sizeof(tint), 0);
        }
        if (!any) {
            frame.cmd.setPipeline(waterVolumePipeline);
            frame.cmd.setBindGroup(0, frameBindGroup);
            any = true;
        }
        frame.cmd.setBindGroup(1, slot->group);
        frame.cmd.setVertexBuffer(0, slot->vertices);
        frame.cmd.draw(6);
    }
    for (auto it = waterQuads.begin(); it != waterQuads.end();) {
        if (!it->seen) {
            it = waterQuads.erase(it); // Unique members self-free
        } else {
            ++it;
        }
    }
}

void WorldRenderer::buildCasterPipelines(rhi::Device& device) {
    // Position-only attributes over the FULL vertex strides (same buffers
    // as the lit pass); depth state mirrors terrain/vegetation casters.
    meshCasterPipeline = { device, device.createPipeline(
        { .shader = shaders->get("shadow_mesh"),
          .vertexBuffers = { render::meshVertexPositionLayout() },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back }) };
    skinnedCasterPipeline = { device, device.createPipeline(
        { .shader = shaders->get("shadow_skinned"),
          .vertexBuffers =
              { { .stride = sizeof(render::SkinnedVertex),
                  .attributes =
                      { { .location = 0,
                          .format = rhi::VertexFormat::F32x3,
                          .offset =
                              offsetof(render::SkinnedVertex, position) },
                        { .location = 4,
                          .format = rhi::VertexFormat::F32x4,
                          .offset = offsetof(render::SkinnedVertex, joints) },
                        { .location = 5,
                          .format = rhi::VertexFormat::F32x4,
                          .offset =
                              offsetof(render::SkinnedVertex, weights) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back }) };
    meshCasterShaderGeneration = shaders->generation("shadow_mesh");
    skinnedCasterShaderGeneration = shaders->generation("shadow_skinned");
}

void WorldRenderer::drawShadowCasters(
    engine::FrameContext& frame, const render::RenderSnapshot& snapshot,
    const RenderView& view, u32 cascade) {
    drawCastersInto(frame, snapshot, view, shadows.casterBindGroup(cascade),
                    cascade == 0);
}

void WorldRenderer::drawCastersInto(engine::FrameContext& frame,
                                        const render::RenderSnapshot& snapshot,
                                        const RenderView& view,
                                        rhi::BindGroupHandle casterGroup,
                                        bool refreshUbos) {
    if (shaders->generation("shadow_mesh") != meshCasterShaderGeneration ||
        shaders->generation("shadow_skinned") !=
            skinnedCasterShaderGeneration) {
        buildCasterPipelines(frame.device);
    }
    const bool firstCascade = refreshUbos;

    // Scene meshes: the per-draw UBO is the lit pass's — the caster pass
    // runs first in the frame, so cascade 0 refreshes the model matrix
    // (drawSceneMeshes rewrites the full block later the same frame).
    if (!snapshot.meshes.empty()) {
        if (meshDraws.size() < snapshot.meshes.size()) {
            meshDraws.resize(snapshot.meshes.size());
        }
        frame.cmd.setPipeline(meshCasterPipeline);
        frame.cmd.setBindGroup(1, casterGroup);
        for (u32 i = 0; i < snapshot.meshes.size(); ++i) {
            const render::RenderSnapshot::MeshInstance& instance =
                snapshot.meshes[i];
            const render::MeshCache::Gpu& mesh =
                view.meshCache->resolve(instance.model);
            MeshDraw& draw = meshDraws[i];
            if (draw.ubo.id() == 0) {
                // std140 ModelUbo: mat4 + tint + info (drawSceneMeshes
                // owns the tail; only the matrix matters here).
                draw.ubo = { frame.device, frame.device.createBuffer(
                    { .usage = rhi::BufferUsage::Uniform,
                      .size = sizeof(Mat4) + 2 * sizeof(Vec4),
                      .dynamic = true },
                    nullptr) };
            }
            if (firstCascade) {
                frame.device.updateBuffer(draw.ubo, &instance.transform,
                                          sizeof(Mat4), 0);
            }
            if (draw.casterGroup.id() == 0) {
                draw.casterGroup = { frame.device, frame.device.createBindGroup(
                    { .entries = { { .binding = 4, .buffer = draw.ubo } } }) };
            }
            frame.cmd.setBindGroup(2, draw.casterGroup);
            frame.cmd.setVertexBuffer(0, mesh.vertices);
            frame.cmd.setIndexBuffer(mesh.indices, rhi::IndexFormat::U32);
            frame.cmd.drawIndexed(mesh.indexCount);
        }
    }

    // Skinned NPCs: model UBO + palette are last frame's (drawNpcs updates
    // them after the cascades) — one frame of shadow lag, invisible at
    // 2048px cascade resolution. Draws from the snapshot; a
    // first-frame NPC has no draw state yet and simply skips one shadow.
    if (!snapshot.skinned.empty()) {
        frame.cmd.setPipeline(skinnedCasterPipeline);
        frame.cmd.setBindGroup(1, casterGroup);
        for (const render::RenderSnapshot::SkinnedInstance& instance :
             snapshot.skinned) {
            SkinnedDraw* slot = nullptr;
            for (SkinnedDraw& draw : skinnedDraws) {
                if (draw.entityId == instance.entityId) {
                    slot = &draw;
                    break;
                }
            }
            if (!slot || slot->modelUbo.id() == 0) {
                continue; // built by drawNpcs later this frame
            }
            if (slot->casterGroup.id() == 0) {
                slot->casterGroup = { frame.device, frame.device.createBindGroup(
                    { .entries = { { .binding = 4,
                                     .buffer = slot->modelUbo },
                                   { .binding = 2,
                                     .buffer = slot->paletteSsbo,
                                     .storage = true } } }) };
            }
            frame.cmd.setBindGroup(2, slot->casterGroup);
            frame.cmd.setVertexBuffer(0, instance.vertices);
            frame.cmd.setIndexBuffer(instance.indices, rhi::IndexFormat::U32);
            frame.cmd.drawIndexed(instance.indexCount);
        }
    }
}

void WorldRenderer::buildMeshPipeline(rhi::Device& device) {
    meshPipeline = { device, device.createPipeline(
        { .shader = shaders->get("mesh"),
          .vertexBuffers = { render::meshVertexLayout() },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Greater }, // reversed-Z
          .cull = rhi::CullMode::Back }) };
    meshShaderGeneration = shaders->generation("mesh");
}

void WorldRenderer::render(engine::FrameContext& frame,
                               const render::RenderSnapshot& snapshot,
                               const RenderView& view) {
    // Resolve last frames' timestamps (never blocking) and
    // open this frame's slot — the scopes below feed the budget table.
    gpuProbe.beginFrame(frame.device);
    // One-shot GPU budget line for headless/scripted sessions (the F6
    // table without eyes on the HUD): logged once past driver warmup,
    // with the rolling window full.
    ++perfFrames;
    if (perfFrames == 2000 && gpuProbe.active()) {
        str line;
        char cell[64];
        for (const render::GpuProbe::PassRow& row : gpuProbe.rows()) {
            std::snprintf(cell, sizeof(cell), " | %s %.2f/%.2f", row.name,
                          row.stats.averageMs, row.stats.maxMs);
            line += cell;
        }
        LOG_INFO("gpu budget (avg/max ms, 120f): frame {:.2f}/{:.2f}{}",
                 gpuProbe.frameAverageMs(), gpuProbe.frameMaxMs(), line);
    }
    shaders->pollHotReload(frame.dt);
    if (cfg.terrain) {
        terrain.refreshPipeline(frame.device, *shaders);
        farTerrain.refreshPipeline(frame.device, *shaders);
    }
    if (cfg.grass) {
        grass.refreshPipeline(frame.device, *shaders);
    }
    if (cfg.vegetation) {
        vegetation.refreshPipeline(frame.device, *shaders);
    }
    if (cfg.sky) {
        sky.refreshPipeline(frame.device, *shaders);
    }
    if (cfg.water && frame.device.caps().copyTexture) {
        water.refreshPipeline(frame.device, *shaders);
    }
    if (cfg.postFx) {
        postFx.refreshPipelines(frame.device, *shaders);
    }
    if (cfg.occlusion) {
        gpuOcclusion.refreshPipelines(frame.device, *shaders);
    }
    if (cfg.gi) {
        radianceCascades.refreshPipelines(frame.device, *shaders);
    }
    if (lightClusters.clusterBuffer().id != 0) {
        lightClusters.refreshPipeline(frame.device, *shaders);
    }
    if (cfg.terrain) {
        terrain.setWireframe(wireframeUi, frame.device, *shaders);
    }
    if (regenerateRequested) {
        regenerateRequested = false;
        if (cfg.terrain) {
            terrain.regenerate(frame.device);
        }
        if (cfg.grass) {
            grass.regenerate(frame.device);
        }
        if (cfg.vegetation) {
            vegetation.regenerate(frame.device, terrain.params.seed);
        }
        occlusion.invalidate();
    }
    // EXPERIMENT A/B (feature/space-colonization-trees): mesh-only swap at
    // the safe point — instance buffers and scatter stay resident.
    if (reseedVegetation) {
        reseedVegetation = false;
        if (cfg.vegetation) {
            vegetation.reseedVariantMeshes(frame.device);
        }
    }
    // Grass panel: a scatter knob moved — re-scatter the meadow only.
    if (grassRescatterRequested) {
        grassRescatterRequested = false;
        if (cfg.grass) {
            grass.regenerate(frame.device);
        }
    }
    // CSM resolution knob: recreate on change; the
    // round-robin then re-renders every cascade next frames (the fresh
    // maps start empty — one frame of unshadowed sun at worst).
    if (static_cast<u32>(shadowResolutionUi) != shadows.resolution()) {
        shadows.destroy(frame.device);
        shadows.create(frame.device,
                       static_cast<u32>(shadowResolutionUi));
        lastCascadesValid = false; // force a full cascade re-render
    }
    // Terrain sculpt: re-mesh JUST the chunks a stroke touched (in place, no
    // hole) — runs live during the stroke for real-time feedback. Grass/veg
    // re-scatter only on commit (`sculptScatterChunks`) so they don't flicker
    // every preview frame. The rest of the world stays put.
    if (cfg.terrain && !sculptDirtyChunks.empty()) {
        terrain.remeshChunks(sculptDirtyChunks);
        sculptDirtyChunks.clear();
    }
    if (!sculptScatterChunks.empty()) {
        if (cfg.grass) {
            grass.invalidateChunks(frame.device, sculptScatterChunks);
        }
        if (cfg.vegetation) {
            vegetation.invalidateChunks(frame.device, sculptScatterChunks);
        }
        sculptScatterChunks.clear();
    }
    if (!view.interiorMode) { // interiors: no terrain/scatter/water to stream
        if (cfg.terrain) {
            core::FrameProbe::Scope probe { *view.probe, "terrain" };
            terrain.update(frame.device, view.camera.position,
                           streamingHold, streamingBoost);
        }
        if (cfg.terrain) {
            // Probed apart from the chunk streaming — a landing bake
            // recreates the 256² map (upload) and must be attributable.
            core::FrameProbe::Scope probe { *view.probe, "lightmap" };
            // Pump/kick the light-map bake (worker; re-bakes on
            // the quantized sun step or when the focus strays).
            terrainLightMap.update(frame.device, terrain.params,
                                   view.camera.position,
                                   shadowSunDirection);
            // Region shading maps (biome/wetness fields for the splat
            // rules; sun-independent, re-bakes on stray or terrain
            // republish).
            terrainShadeMap.update(frame.device, terrain.params,
                                   view.camera.position);
        }
        if (cfg.terrain && cfg.postFx) {
            // Valley data for the ground mist (sun-independent; re-bakes
            // only when the camera strays half a kilometer).
            core::FrameProbe::Scope probe { *view.probe, "mistmap" };
            mistMap.update(frame.device, terrain.params,
                           view.camera.position);
        }
        if (cfg.terrain && farTerrainUi) {
            // Distant silhouettes: coarse 12 km mesh, worker-baked.
            core::FrameProbe::Scope probe { *view.probe, "farterrain" };
            farTerrain.update(frame.device, terrain.params,
                              view.camera.position,
                              cfg.vegetation
                                  ? vegetation.treeSilhouette()
                                  : render::VegetationSystem::
                                        TreeSilhouette {},
                              { terrain.layerAlbedoBase(0),
                                terrain.layerAlbedoBase(1),
                                terrain.layerAlbedoBase(2),
                                terrain.layerAlbedoBase(3),
                                terrain.layerAlbedoBase(4) });
        }
        // Height-horizon occlusion: rebuilt on a worker
        // whenever the camera strays; stays valid (conservative) meanwhile.
        if (cfg.occlusion) {
            core::FrameProbe::Scope probe { *view.probe, "occlusion" };
            occlusion.pump();
            occlusion.configure(static_cast<f32>(terrain.viewRadius) *
                                render::TerrainSystem::kChunkSize);
            if (occlusion.wantsRebuild(view.camera.position)) {
                occlusion.rebuild(terrain.params, view.camera.position,
                                  terrain.chunkTops());
            }
        }
        if (cfg.gi) {
            // The GI's albedo tile bounces the same tinted ground.
            radianceCascades.terrainTintStrength = view.terrainTintStrength;
        }
        if (cfg.grass) {
            core::FrameProbe::Scope probe { *view.probe, "grass" };
            // Root-albedo bake follows the terrain's splat scale,
            // macro-tint strength and the ACTIVE material set's variant
            // means (one ground-color source, cooked A/B included).
            bool rootChanged = false;
            for (u32 v = 0; v < 4; ++v) {
                if (grass.scatterTuning.rootAlbedoBase[v] !=
                    terrain.grassAlbedoBase(v)) {
                    grass.scatterTuning.rootAlbedoBase[v] =
                        terrain.grassAlbedoBase(v);
                    rootChanged = true;
                }
            }
            if (grass.scatterTuning.splatUvScale != view.splatUvScale ||
                grass.scatterTuning.tintStrength !=
                    view.terrainTintStrength ||
                rootChanged) {
                grass.scatterTuning.splatUvScale = view.splatUvScale;
                grass.scatterTuning.tintStrength = view.terrainTintStrength;
                grass.regenerate(frame.device);
            }
            grass.update(frame.device, terrain.params,
                         view.camera.position, streamingHold);
        }
        if (cfg.vegetation) {
            core::FrameProbe::Scope probe { *view.probe, "veg" };
            vegetation.update(frame.device, terrain.params,
                              view.camera.position, streamingHold);
        }
        if (cfg.water && frame.device.caps().copyTexture) {
            core::FrameProbe::Scope probe { *view.probe, "water" };
            water.update(frame.device, terrain.params,
                         view.camera.position);
        }
    }

    const render::Camera3D& camera = view.camera;
    const Mat4 viewProj = camera.viewProj(frame.aspect);
    // CPU chunk culling: one frustum per rendered viewpoint.
    const render::Frustum viewFrustum = render::Frustum::fromViewProj(viewProj);
    const render::SkySystem::SkyState skyState =
        sky.evaluate({ .cloudCoverage = view.atmos.cloudCoverage,
                       .sunIntensity = view.atmos.sunIntensity,
                       .ambientIntensity = view.atmos.ambientIntensity,
                       .saturation = view.atmos.saturation,
                       .warmth = view.atmos.warmth });

    // Shadows ramp out as the sun crosses the horizon (no sun, no shadows),
    // and soften away under heavy cloud cover (diffuse light casts none).
    const bool shadowsAvailable = shadows.receiverBindGroup().id != 0;
    const f32 shadowStrength =
        (shadowsUi && shadowsAvailable && !view.interiorMode)
            ? glm::smoothstep(-0.02f, 0.06f, skyState.sunDirection.y) *
                  (1.0f - 0.65f * view.atmos.cloudCoverage)
            : 0.0f;
    // The cascades use a QUANTIZED sun (otherwise tree shadows tremble).
    // The texel snap absorbs camera translation, but the game clock spins
    // the light a fraction of a degree every frame, re-basing the snap —
    // the edges crawl. Hysteresis instead: shadows sit rock-stable, then
    // take an imperceptible ~0.4° step every ~8 real seconds; the VISIBLE
    // sun/lighting keeps moving smoothly.
    bool sunStepped = false;
    if (glm::dot(shadowSunDirection, skyState.sunDirection) <
        std::cos(glm::radians(0.4f))) {
        shadowSunDirection = skyState.sunDirection;
        sunStepped = true;
    }
    render::ShadowMapper::Cascades cascades {};
    // Which cascades actually re-render this frame (docs/RENDERING.md):
    // most of the shadow cost is the two
    // far cascades. Round-robin: cascade 0 every frame, cascades 1 and 2
    // on alternate frames, each keeping its PREVIOUS matrix when skipped
    // (the stale depth must be sampled with the matrix it was drawn
    // with). A sun step (or the A/B toggle off) re-renders all three.
    array<bool, render::ShadowMapper::kCascadeCount> cascadeDue {
        true, true, true
    };
    if (shadowStrength > 0.0f) {
        cascades = shadows.computeCascades(camera, frame.aspect,
                                           shadowSunDirection);
        ++shadowFrame;
        const bool full =
            !shadowRoundRobinUi || !lastCascadesValid || sunStepped;
        if (!full) {
            for (u32 i = 1; i < render::ShadowMapper::kCascadeCount; ++i) {
                cascadeDue[i] = (shadowFrame + i) % 2 == 0;
                if (!cascadeDue[i]) {
                    cascades.viewProj[i] = lastCascades.viewProj[i];
                    cascades.splitFar[i] = lastCascades.splitFar[i];
                    cascades.texelWorld[i] = lastCascades.texelWorld[i];
                }
            }
        }
        lastCascades = cascades;
        lastCascadesValid = true;
        shadows.updateCascadeUbos(frame.device, cascades);
    } else {
        lastCascadesValid = false; // interiors/night: refit from scratch
    }

    // Planar reflection is meaningful only from above the surface —
    // and (auto-skip) only when some water can actually show: a
    // RESIDENT chunk dipping below sea level inside the frustum. The
    // known miss: sea at the horizon beyond the resident ring (~960 m) —
    // the A/B toggle is there for that exact check.
    bool waterVisible = true;
    if (cfg.water && reflectionAutoSkipUi && !view.interiorMode &&
        reflectionsUi) {
        waterVisible = false;
        terrain.collectChunkAabbs(occlusionAabbs);
        const f32 sea = terrain.params.seaLevel;
        for (const auto& aabb : occlusionAabbs) {
            if (aabb.lo.y >= sea) {
                continue; // fully above the water table
            }
            // Beyond ~600 m the mirror sits deep in the fog (fogStart
            // 450): not worth a full scene re-render (perf audit
            // 2026-08-06 — 7 ms while no readable water was in sight).
            const Vec2 toChunk {
                (aabb.lo.x + aabb.hi.x) * 0.5f - camera.position.x,
                (aabb.lo.z + aabb.hi.z) * 0.5f - camera.position.z
            };
            if (glm::dot(toChunk, toChunk) > 600.0f * 600.0f) {
                continue;
            }
            // Test the chunk's water RECTANGLE (the sea plane spans it).
            if (viewFrustum.intersectsAabb(
                    { aabb.lo.x, sea - 1.0f, aabb.lo.z },
                    { aabb.hi.x, sea + 1.0f, aabb.hi.z })) {
                waterVisible = true;
                break;
            }
        }
    }
    const bool reflectionsActive =
        reflectionsUi && reflectionFb.id() != 0 && !view.interiorMode &&
        camera.position.y > terrain.params.seaLevel && waterVisible;

    // GI: decide this frame's inject + snap the grid origins
    // BEFORE the UBO composition so uGiGridInfo matches the volume
    // content the apply will sample (the moving-halo lag fix).
    if (cfg.gi) {
        radianceCascades.prepare(camera.position);
    }

    // The whole UBO composition is pure: gather the inputs,
    // let the composer build both variants, upload. This side keeps only
    // the GPU-availability gates and the updateBuffer calls.
    const ComposedFrame composed = composeFrameUniforms({
        .viewProj = viewProj,
        .cameraPosition = camera.position,
        .width = offscreenWidth != 0 ? offscreenWidth : frame.width,
        .height = offscreenHeight != 0 ? offscreenHeight : frame.height,
        .dt = frame.dt,
        .timeSeconds = view.timeSeconds,
        .sky = skyState,
        .cascades = cascades,
        .shadowStrength = shadowStrength,
        .atmos = view.atmos,
        .interiorMode = view.interiorMode,
        .interiorAmbient = view.interiorAmbient,
        .interiorDaylightWeight =
            interiorDaylightWeightUi *
            aboveBuried(view.camera.position.y, view.buriedBelowY),
        .froxelFog = cfg.froxels && postFx.froxelFog && postFx.froxelReady(),
        .interiorDustDensity = interiorDustDensityUi,
        .seaLevel = terrain.params.seaLevel,
        .snowLine = view.snowLine,
        .splatUvScale = view.splatUvScale,
        .splatBlendDepth = view.splatBlendDepth,
        .terrainTintStrength = view.terrainTintStrength,
        .splatDetailFade = view.splatDetailFade,
        .pomDistance = view.pomDistance,
        .splatVariety = view.splatVariety,
        .pomShadowStrength = view.pomShadowStrength,
        .pomDepth = view.pomDepth,
        .barkEnabled = vegetation.barkLoaded(),
        .ssaoStrength = ssaoStrengthUi,
        .ssaoRadius = ssaoRadiusUi,
        .ssdmAmplitude = ssdmModeUi != 0 ? ssdmAmpUi : 0.0f,
        .shadowFarUvScale = render::ShadowMapper::kFarCascadeScale,
        .reflectionsActive = reflectionsActive,
        // Horizon closure: at the far mesh's reach when it stands in,
        // else at the streaming ring. z of the same uniform carries the
        // ring for the far mesh's sink bias.
        .drawDistance =
            cfg.terrain
                ? (farTerrainUi && farTerrain.ready()
                       ? farTerrain.reach()
                       : static_cast<f32>(terrain.viewRadius) *
                             render::TerrainSystem::kChunkSize)
                : 0.0f,
        .nearRingDistance = cfg.terrain
                                ? static_cast<f32>(terrain.viewRadius) *
                                      render::TerrainSystem::kChunkSize
                                : 0.0f,
        .treeFadeEnd = vegetation.treeFadeEnd(),
        .seasonAutumn = seasonAutumnUi,
        .seasonLeafFall = seasonLeafFallUi,
        .leafSeason = vegetation.leafSeason(),
        .debugBuffer = debugBufferUi,
        .stylized = stylizedUi,
        .tonemap = tonemapUi,
        .exposure = exposureUi,
        .cascadeDebug = cascadeDebugUi,
        .grading = gradingUi,
        .gradeVibrance = gradeVibranceUi,
        .gradeSplitTone = gradeSplitToneUi,
        .gradeContrast = gradeContrastUi,
        .autoExposure = autoExposureUi,
        .autoExposureMin = autoExposureMinUi,
        .autoExposureMax = autoExposureMaxUi,
        .waterMapInfo = water.poolMapInfo(),
        .waterInfoMapInfo = water.infoMapInfo(),
        .terrainShadeMapInfo = terrainShadeMap.info(),
        .terrainLightInfo = terrainLightMap.info(),
        .terrainLightActive =
            terrainLightUi && !view.interiorMode && terrainLightMap.ready(),
        .waterSurfaceY = effectiveWaterSurfaceY(snapshot, view),
        .windTime = view.windTime,
        .grassBend = view.grassBend,
        .playerFeet = view.playerFeet,
        .grassShapeInfo = { grass.renderTuning.bladeHeight,
                            grass.renderTuning.bladeHalfWidth,
                            grass.renderTuning.detailNear,
                            grass.renderTuning.detailFar },
        .grassLodInfo = { grass.renderTuning.thinStart,
                          grass.renderTuning.thinEnd,
                          grass.renderTuning.farDensity,
                          grass.renderTuning.widthCompensation },
        .grassBaseTint = { grass.renderTuning.baseTint,
                           grass.renderTuning.fadeStart },
        .grassTipTint = { grass.renderTuning.tipTint,
                          grass.renderTuning.fadeEnd },
        .grassShadeInfo = { grass.renderTuning.rootAo,
                            grass.renderTuning.sheen,
                            grass.renderTuning.bladeNormals, 0.0f },
        .grassBladeInfo = { grass.renderTuning.brightMin,
                            grass.renderTuning.brightMax,
                            grass.renderTuning.middleDarken,
                            grass.renderTuning.backscatter },
        .leafLodInfo = { vegetation.colonizedTreeParams.leafSolidStart,
                         vegetation.colonizedTreeParams.leafSolidEnd, 0.0f,
                         0.0f },
        .stylizedDiffuseInfo = stylizedDiffuseUi,
        .stylizedShadowInfo = stylizedShadowUi,
        .stylizedSpecInfo = stylizedSpecUi,
        // giInfo() gates on ready() itself (interiors included).
        .giInfo = radianceCascades.giInfo(),
        .giGridInfo = radianceCascades.giGridInfo(),
        .giBandInfo = { radianceCascades.tuning.bandCount,
                        radianceCascades.tuning.bandAa,
                        radianceCascades.tuning.giFloor, 0.0f },
        .mistActive = mistUi && mistMap.ready(),
        .mistCoverageSoftness = mistCoverageSoftnessUi,
        .mistReach = mistReachUi,
        .mistShapeInfo = mistShapeUi,
        .mistMapInfo = mistMap.info(),
        .mistDetailInfo = { noiseVolume.ready() && mistNoiseTexUi ? 1.0f
                                                                  : 0.0f,
                            static_cast<f32>(mistStepsUi),
                            mistDetailDropoutUi, mistSunBoostUi },
        .mistLightInfo = mistLightUi,
        .cloudVolInfo = { skyCloudsUi && noiseVolume.ready() ? 1.0f : 0.0f,
                          skyCloudShapeUi.x, skyCloudShapeUi.y,
                          skyCloudShapeUi.z },
        .cloudVolLightInfo = skyCloudLightUi,
        .cloudVolShapeInfo = { skyCloudShapeUi.w, skyCloudLiningLobeUi,
                               skyCloudPowderUi, skyCloudPuffinessUi },
        .cloudVolRimInfo = { skyCloudRimGainUi, skyCloudRimLobeUi,
                             skyCloudBaseDarkUi, 0.0f },
        .mistPuffInfo = { mistPuffinessUi, 0.0f, 0.0f, 0.0f },
        .waterDebugInfo = { static_cast<f32>(waterDebugUi), 0.0f, 0.0f,
                            0.0f },
    });
    const render::FrameUniforms& uniforms = composed.base;
    render::FrameUniforms frameData = composed.resolved;
    // Clustered forward (docs/RENDERING.md §5): the grid's far reach is
    // the froxel slicing's (interior room scale / exterior CSM reach) so
    // both grids share their z slices by construction.
    const bool clustered = clusteredLightsUi && lightClusters.ready();
    frameData.clusterInfo = { clustered ? 1.0f : 0.0f,
                              render::volumetricReach(view.interiorMode,
                                                      view.atmos.fogStart),
                              0.0f, 0.0f };
    if (cfg.sky && frameData.stormInfo.y > 0.003f) {
        frame.device.updateBuffer(rainOcclusionUbo,
                                  &frameData.rainOcclusionViewProj,
                                  sizeof(Mat4), 0);
    }

    // Key-shadow atlas selection (docs/RENDERING.md §5 B6): the up-to-4
    // best-scored castsShadow lights get a 1024² tile. Selected HERE so
    // the tile matrices ride this frame-UBO upload and the per-light
    // slot lands in the lights UBO below (windowInfo.z). The tiles
    // themselves render after the cloud bake.
    struct KeyShadowPick {
        Vec3 anchor; // the light's original position — the UBO match key
        Mat4 viewProj;
    };
    vector<KeyShadowPick> keyShadowPicks;
    if (keyShadowUi && meshShadowCastersUi) {
        struct KeyCandidate {
            f32 score;
            Vec3 anchor;
            Vec3 position;
            Vec3 direction;
            f32 fov;
            f32 radius;
        };
        vector<KeyCandidate> keyCandidates;
        const f32 keySunGate =
            glm::smoothstep(0.05f, 0.20f, shadowSunDirection.y);
        for (const render::SceneLight& light : snapshot.shadowLights) {
            Vec3 position = light.position;
            Vec3 direction = light.direction;
            if (light.sunLinked) {
                // Same anchor model as the lights UBO: the shadow camera
                // sits OUTSIDE, films through the window along the live
                // beam — the aperture clips the pool for real. A dead
                // beam (night, sun behind the wall) frees its slot.
                direction = -shadowSunDirection;
                const f32 facing = glm::dot(
                    direction, glm::normalize(light.direction));
                if (keySunGate * glm::smoothstep(0.15f, 0.40f, facing) <=
                    0.05f) {
                    continue;
                }
                position += shadowSunDirection * 3.5f;
            } else if (light.spotAngle <= 0.0f) {
                // POINT lights film straight DOWN: one cone cannot
                // cover the sphere, and the ground disc under the lamp
                // is where cast shadows read (an unrotated light's
                // default +Z cone missed everything beside it — "the
                // meshes by the door cast nothing"). Wall shadows above
                // the lamp's own height are the accepted loss.
                direction = Vec3 { 0.0f, -1.0f, 0.0f };
            }
            const Vec3 d = light.position - camera.position;
            keyCandidates.push_back(
                { light.intensity / (1.0f + glm::dot(d, d)),
                  light.position, position, direction,
                  light.spotAngle > 0.0f
                      ? glm::min(light.spotAngle * 1.3f, 150.0f)
                      : 150.0f,
                  light.radius });
        }
        std::stable_sort(keyCandidates.begin(), keyCandidates.end(),
                         [](const KeyCandidate& a, const KeyCandidate& b) {
                             return a.score > b.score;
                         });
        const size_t slots =
            glm::min<size_t>(keyCandidates.size(), kKeyShadowSlots);
        for (size_t slot = 0; slot < slots; ++slot) {
            const KeyCandidate& pick = keyCandidates[slot];
            const Vec3 up = std::abs(pick.direction.y) > 0.95f
                                ? Vec3 { 1.0f, 0.0f, 0.0f }
                                : Vec3 { 0.0f, 1.0f, 0.0f };
            const Mat4 lightView =
                glm::lookAt(pick.position, pick.position + pick.direction,
                            up);
            const Mat4 proj = glm::perspective(glm::radians(pick.fov),
                                               1.0f, 0.05f, pick.radius);
            frameData.keyShadowAtlasViewProj[slot] = proj * lightView;
            keyShadowPicks.push_back(
                { pick.anchor, frameData.keyShadowAtlasViewProj[slot] });
        }
    }
    frame.device.updateBuffer(frameUbo, &frameData, sizeof(frameData), 0);

    // The selected local lights, flicker applied CPU-side (sin +
    // per-index phase — cheap and stateless). Only the clustered path
    // may consume the full budget; the legacy per-pixel loop stays
    // capped at kFallbackLights (its cost is per-slot x per-pixel).
    {
        const u32 budget = clustered ? kMaxLights : kFallbackLights;
        LightsUniforms lights;
        const vector<render::SceneLight>& nearest = snapshot.lights;
        // G7b — the penumbra experiment: with "lights via RC only", the
        // DIRECT contribution is cut (count 0 -> localLights() adds
        // nothing) and the lights exist purely in the GI volume: their
        // occlusion and penumbras come from the cascades, at voxel
        // resolution. Only meaningful while the RC technique is active.
        const bool rcOnly =
            radianceCascades.tuning.rcOnlyLights &&
            radianceCascades.tuning.technique ==
                render::GiTechnique::RadianceCascades;
        // H2 (docs/RENDERING.md): sun-linked sources take the SUN's
        // live color — hour and weather included — and die below the
        // horizon. WINDOW projectors (docs/RENDERING.md) keep the anchor
        // and carry the window's NORMAL + extents; the facing gate
        // lives in the shader (beam · normal). Plain sun-linked spots
        // keep the pushed-cone model. rcOnly lights skip the direct
        // path entirely — their GI blob carries them.
        const f32 sunGate =
            glm::smoothstep(0.05f, 0.20f, shadowSunDirection.y);
        const Vec3 sunTint { uniforms.sunColor };
        u32 slot = 0;
        for (u32 i = 0; i < nearest.size() && slot < budget && !rcOnly;
             ++i) {
            const render::SceneLight& light = nearest[i];
            if (light.rcOnly) {
                continue;
            }
            const bool window = light.windowHalfWidth > 0.0f;
            f32 intensity = light.intensity;
            Vec3 color = light.color;
            Vec3 position = light.position;
            if (light.sunLinked) {
                color = sunTint;
                intensity *= sunGate * aboveBuried(light.position.y,
                                                   view.buriedBelowY);
                if (!window) {
                    const f32 facing =
                        glm::dot(-shadowSunDirection,
                                 glm::normalize(light.direction));
                    intensity *=
                        glm::smoothstep(0.15f, 0.40f, facing);
                    position += shadowSunDirection * 3.5f;
                }
            }
            if (light.flicker > 0.0f) {
                const f32 phase = static_cast<f32>(i) * 1.7f;
                intensity *=
                    1.0f + light.flicker *
                               (0.55f * std::sin(view.timeSeconds * 9.0f + phase) +
                                0.45f * std::sin(view.timeSeconds * 23.0f +
                                                 phase * 3.1f));
            }
            lights.positionRadius[slot] = { position, light.radius };
            lights.colorIntensity[slot] = { color * intensity, 0.0f };
            const bool spot = light.spotAngle > 0.0f;
            lights.directionAngle[slot] = {
                glm::normalize(window ? light.direction
                               : light.sunLinked ? -shadowSunDirection
                                                 : light.direction),
                window ? -3.0f
                : spot ? std::cos(glm::radians(light.spotAngle * 0.5f))
                       : -2.0f
            };
            // The key-shadow atlas slot (windowInfo.z, 0 = unshadowed):
            // matched against the pick's anchor — both come from the
            // same Transform, so the positions are bit-equal.
            f32 shadowSlot = 0.0f;
            for (size_t k = 0; k < keyShadowPicks.size(); ++k) {
                if (glm::distance(keyShadowPicks[k].anchor,
                                  light.position) < 0.05f) {
                    shadowSlot = static_cast<f32>(k + 1);
                    break;
                }
            }
            lights.windowInfo[slot] = { light.windowHalfWidth,
                                        light.windowHalfHeight, shadowSlot,
                                        0.0f };
            ++slot;
        }
        lights.count.x = static_cast<f32>(slot);
        frame.device.updateBuffer(lightsUbo, &lights, sizeof(lights), 0);
    }

    // Cluster the UBO's lights for this frame (the frame bind group —
    // bound by every consumer — carries the list SSBO at binding 4).
    if (clustered) {
        render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                      "clusterCull" };
        frame.cmd.setBindGroup(0, frameBindGroup);
        lightClusters.run(frame.cmd);
    }

    // Bake this frame's cloud field before anything lights with it.
    if (cfg.sky && !view.interiorMode) {
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "cloudBake" };
        sky.bakeCloudMap(frame.cmd, frameBindGroup);
    }

    // Key-shadow atlas tiles (§5 B6): one clear, one 1024² viewport per
    // shadowed light, each tile through its own caster UBO/group.
    if (!keyShadowPicks.empty()) {
        render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                      "keyShadow" };
        for (size_t slot = 0; slot < keyShadowPicks.size(); ++slot) {
            frame.device.updateBuffer(keyShadowUbos[slot],
                                      &keyShadowPicks[slot].viewProj,
                                      sizeof(Mat4), 0);
        }
        frame.cmd.beginRenderPass({ .framebuffer = keyShadowFb,
                                    .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::Clear });
        for (size_t slot = 0; slot < keyShadowPicks.size(); ++slot) {
            frame.cmd.setViewport(static_cast<u32>(slot & 1) * 1024,
                                  static_cast<u32>(slot >> 1) * 1024,
                                  1024, 1024);
            drawCastersInto(frame, snapshot, view,
                            keyShadowCasterGroups[slot],
                            /*refreshUbos=*/slot == 0);
        }
        frame.cmd.endRenderPass();
    }

    // The top-down rain occlusion depth (roof + canopy cover).
    if (cfg.sky && frameData.stormInfo.y > 0.003f && meshShadowCastersUi) {
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "rainOcc" };
        frame.cmd.beginRenderPass({ .framebuffer = rainOcclusionFb,
                                    .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::Clear });
        drawCastersInto(frame, snapshot, view, rainCasterGroup,
                        /*refreshUbos=*/true);
        // Trees shelter too: the vegetation caster path through the
        // top-down matrix. Billboards orient to THEIR pass's matrix
        // (shadow_prop.vert reads the bound ShadowUbo), so cards face up
        // here; solid shadow proxies / ultra lobes keep it cutout-free.
        // The window is 40 m around the camera — one chunk of reach.
        if (cfg.vegetation) {
            vegetation.drawDepth(frame.cmd, frameBindGroup,
                                 rainCasterGroup, camera.position,
                                 /*maxChunkDistance=*/1, nullptr,
                                 /*ultraDetail=*/true);
        }
        frame.cmd.endRenderPass();
    }

    // Cascade passes: depth-only casters from the sun's point of view.
    if (shadowStrength > 0.0f) {
        core::FrameProbe::Scope probe { *view.probe, "shadows" };
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "shadows" };
        for (u32 i = 0; i < render::ShadowMapper::kCascadeCount; ++i) {
            if (!cascadeDue[i]) {
                continue; // kept last frame's depth AND matrix
            }
            // Cull casters against THIS cascade's ortho volume — the
            // near cascades cover a fraction of the 9-chunk ring, and the
            // CSM cost is vertex-bound.
            const render::Frustum cascadeFrustum =
                render::Frustum::fromViewProj(cascades.viewProj[i]);
            frame.cmd.beginRenderPass(
                { .framebuffer = shadows.framebuffer(i),
                  .loadOp = rhi::LoadOp::DontCare,
                  .depthLoadOp = rhi::LoadOp::Clear });
            // Far cascades render into a quarter viewport (the 1024
            // that pays for the doubled reach); the last one also
            // extends its caster ring to its 1600 m split.
            const u32 effRes = shadows.effectiveResolution(i);
            frame.cmd.setViewport(0, 0, effRes, effRes);
            frame.cmd.setScissor(0, 0, effRes, effRes);
            const i32 casterChunks =
                i + 1 == render::ShadowMapper::kCascadeCount ? 26 : 13;
            if (cfg.terrain) {
                terrain.drawDepth(frame.cmd, shadows.casterBindGroup(i),
                                  camera.position, casterChunks,
                                  &cascadeFrustum);
            }
            // Same 13-chunk cap: the last cascade ends at 800 m (the
            // ultra tree ring). Far cascades cast with the solid shadow
            // proxies (metaball blobs), cascade 0 with the leafy cards.
            if (cfg.vegetation) {
                vegetation.drawDepth(frame.cmd, frameBindGroup,
                                     shadows.casterBindGroup(i),
                                     camera.position, casterChunks,
                                     &cascadeFrustum,
                                     /*ultraDetail=*/i > 0);
            }
            // Scene meshes + NPCs join the casters (A/B toggle).
            if (meshShadowCastersUi) {
                drawShadowCasters(frame, snapshot, view, i);
            }
            frame.cmd.endRenderPass();
        }
    }

    // GI voxel clipmap re-injection (docs/RENDERING.md) — its
    // HISTORICAL slot: after the CSM passes (the inject samples fresh
    // shadow maps), outside any render pass. Pipelined (PG2), the chain
    // records at the END of the frame instead and this slot only fences
    // LAST frame's cascade 0 for this frame's consumers — everything
    // recorded above (cluster cull, cloud bake, CSM, key shadows) reads
    // no GI and overlaps the still-running chain.
    if (cfg.gi) {
        if (!radianceCascades.tuning.pipelined) {
            recordGiUpdate(frame, snapshot, view, uniforms, clustered);
        } else if (radianceCascades.applyGroup().id != 0 &&
                   !(radianceCascades.tuning.asyncCompute &&
                     frame.device.caps().asyncCompute)) {
            // Async: the cross-queue semaphore already publishes last
            // frame's chain to this frame's consumers — no fence needed.
            frame.cmd.memoryBarrier(rhi::BarrierStage_Fragment |
                                    rhi::BarrierStage_Compute);
        }
    }

    const bool useOffscreen = frame.device.caps().offscreenTargets;
    if (useOffscreen) {
        // Internal 3D resolution (renderScaleUi): the scene renders into
        // a scaled offscreen target; the tonemap composite upscales to
        // the native backbuffer where the UI stays crisp.
        const u32 sceneW = glm::max(
            8u, static_cast<u32>(std::lround(static_cast<f64>(frame.width) *
                                             renderScaleUi)));
        const u32 sceneH = glm::max(
            8u,
            static_cast<u32>(std::lround(static_cast<f64>(frame.height) *
                                         renderScaleUi)));
        ensureOffscreenTarget(frame.device, sceneW, sceneH);
        if (shaders->generation(kTonemapShader) != blitShaderGeneration) {
            rebuildBlitPipeline(frame.device);
        }
    }

    // Planar reflection: the scene mirrored about the water plane, at half
    // resolution. The mirrored view flips triangle winding (front face CW),
    // and an oblique near plane clips everything below the surface.
    if (reflectionsActive) {
        const f32 waterY = terrain.params.seaLevel;
        Mat4 mirror { 1.0f };
        mirror[1][1] = -1.0f;
        mirror[3][1] = 2.0f * waterY;
        const Mat4 reflectedView = camera.view() * mirror;
        // Keep the above-water side; tiny epsilon avoids a clipped seam
        // right at the waterline.
        const Vec4 planeWorld { 0.0f, 1.0f, 0.0f, -(waterY - 0.08f) };
        const Vec4 planeView =
            glm::transpose(glm::inverse(reflectedView)) * planeWorld;
        const Mat4 reflectedProj =
            render::obliqueProjection(camera.proj(frame.aspect), planeView);
        const Mat4 reflectedViewProj = reflectedProj * reflectedView;
        // Cull with the NON-oblique projection: Lengyel's trick corrupts
        // the far plane, and the regular frustum is a superset (safe).
        const render::Frustum reflectionFrustum = render::Frustum::fromViewProj(
            camera.proj(frame.aspect) * reflectedView);

        core::FrameProbe::Scope reflectionProbe { *view.probe, "reflection" };
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "reflection" };
    render::FrameUniforms reflectionUniforms = uniforms;
        reflectionUniforms.viewProj = reflectedViewProj;
        reflectionUniforms.invViewProj = glm::inverse(reflectedViewProj);
        reflectionUniforms.cameraPos = { camera.position.x,
                                         2.0f * waterY - camera.position.y,
                                         camera.position.z, 1.0f };
        // Billboard leaf cards re-aim at the MIRRORED camera in
        // tree.vert, so unlike static geometry their screen winding does
        // NOT flip with this pass's clockwise front face — they would be
        // back-face culled (leafless reflected trees). The flag makes
        // tree.vert flip the card corners to match.
        reflectionUniforms.leafLodInfo.z = 1.0f;
        frame.device.updateBuffer(reflectionUbo, &reflectionUniforms,
                                  sizeof(reflectionUniforms), 0);

        // The mirror's depth is pure scaffolding for this pass's own depth
        // test — nothing ever samples it, so it never leaves the tile.
        frame.cmd.beginRenderPass(
            { .framebuffer = reflectionFb,
              .loadOp = rhi::LoadOp::DontCare,
              .depthLoadOp = rhi::LoadOp::Clear,
              .clearDepth = 0.0f, // reversed-Z far
              .depthStoreOp = rhi::StoreOp::DontCare });
        frame.cmd.setFrontFace(rhi::FrontFace::Clockwise);
        if (sky.cloudMapBindGroup().id != 0) {
            frame.cmd.setBindGroup(3, sky.cloudMapBindGroup());
        }
        if (terrainLightMap.bindGroup().id != 0) {
            frame.cmd.setBindGroup(4, terrainLightMap.bindGroup());
        }
        if (terrainShadeMap.bindGroup().id != 0) {
            frame.cmd.setBindGroup(7, terrainShadeMap.bindGroup());
        }
        if (cfg.terrain) {
            terrain.draw(frame.cmd, reflectionBindGroup,
                         shadows.receiverBindGroup(), &reflectionFrustum);
        }
        // Trees only: rocks and bushes are invisible in a wobbly half-res
        // reflection — low-detail canopies for the same reason.
        if (cfg.vegetation) {
            vegetation.draw(frame.cmd, reflectionBindGroup,
                            shadows.receiverBindGroup(),
                            render::VegetationSystem::kTreeVariants,
                            camera.position, /*forceLowDetail=*/true,
                            &reflectionFrustum);
        }
        if (cfg.sky) {
            sky.draw(frame.cmd, reflectionBindGroup);
        }
        frame.cmd.endRenderPass();
    }

    // (The Hi-Z verdict readback is gone — docs/RENDERING.md §6.0 I6:
    // the cull's verdict lives in the indirect commands and never
    // crosses the CPU. The legacy draw path keeps the CPU horizon
    // occlusion only.)

    // Exterior: the sky covers every background pixel — no color clear.
    // Interior: clear to a near-black room tone instead.
    {
        core::FrameProbe::Scope probe { *view.probe, "mainPass" };
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "mainPass" };
        frame.cmd.beginRenderPass(
            { .framebuffer =
                  useOffscreen ? offscreenFb : rhi::FramebufferHandle {},
              .loadOp =
                  view.interiorMode ? rhi::LoadOp::Clear : rhi::LoadOp::DontCare,
              .clearColor = { 0.015f, 0.014f, 0.013f, 1.0f },
              .depthLoadOp = rhi::LoadOp::Clear,
              .clearDepth = 0.0f }); // reversed-Z far
        if (sky.cloudMapBindGroup().id != 0) {
            frame.cmd.setBindGroup(3, sky.cloudMapBindGroup());
        }
        if (terrainLightMap.bindGroup().id != 0) {
            frame.cmd.setBindGroup(4, terrainLightMap.bindGroup());
        }
        if (keyShadowReceiverGroup.id() != 0) {
            frame.cmd.setBindGroup(5, keyShadowReceiverGroup);
        }
        if (radianceCascades.applyGroup().id != 0) {
            // The merged GI cascade 0 for gi.glsl (unit 11).
            frame.cmd.setBindGroup(6, radianceCascades.applyGroup());
        }
        if (terrainShadeMap.bindGroup().id != 0) {
            frame.cmd.setBindGroup(7, terrainShadeMap.bindGroup());
        }
        // Occlusion applies to the main view only: the set is built for
        // the real camera, not the mirrored one (the grass ring is too
        // close to ever be ridge-occluded — frustum only). CPU horizon
        // only — the GPU verdict drives the indirect commands directly.
        combinedOccluded.clear();
        if (occlusionUi && occlusion.occludedSet()) {
            combinedOccluded = *occlusion.occludedSet();
        }
        const std::unordered_set<u64>* occludedSet =
            combinedOccluded.empty() ? nullptr : &combinedOccluded;
        if (!view.interiorMode) {
            // Sub-probes only where they MEASURE: inside a pass, Metal
            // executes the whole encoder as one tiled unit and mid-pass
            // timestamps collapse (~0.01 ms) — the midPassTimestamps cap
            // gates them.
            // The geometry counters (perf panel) carry the dissection on
            // Vulkan instead.
            render::GpuProbe* subProbe =
                frame.device.caps().midPassTimestamps ? &gpuProbe : nullptr;
            rhi::Device* subDevice =
                subProbe != nullptr ? &frame.device : nullptr;
            if (cfg.terrain && farTerrainUi) {
                // Far silhouettes FIRST: everything nearer overdraws
                // them by depth; they extend the world past the ring.
                farTerrain.draw(frame.cmd, frameBindGroup,
                                sky.cloudMapBindGroup());
            }
            // GPU-driven path (docs/RENDERING.md §6.0): consume the
            // indirect commands the cull dispatch wrote LAST frame — the
            // verdict never crossed the CPU. Falls back to the per-chunk
            // loops whenever the commands aren't fresh (interiors, first
            // frames, toggle off, candidate overflow).
            const bool indirectDraw =
                gpuIndirectUi && gpuOcclusionUi &&
                frame.device.caps().multiDrawIndirect &&
                occlusionCommandsFresh && gpuOcclusion.commandsValid();
            if (cfg.terrain) {
                render::GpuProbe::Scope sub { subProbe, subDevice,
                                              "mainTerrain" };
                if (indirectDraw) {
                    terrain.drawIndirect(frame.cmd, frameBindGroup,
                                         shadows.receiverBindGroup(),
                                         gpuOcclusion.commandBuffer(),
                                         gpuOcclusion.groupFirst().data(),
                                         gpuOcclusion.groupCount().data());
                } else {
                    terrain.draw(frame.cmd, frameBindGroup,
                                 shadows.receiverBindGroup(), &viewFrustum,
                                 occludedSet);
                }
            }
            if (cfg.vegetation) {
                render::GpuProbe::Scope sub { subProbe, subDevice,
                                              "mainVeg" };
                if (indirectDraw && !vegetation.showcaseActive()) {
                    vegetation.drawIndirect(
                        frame.cmd, frameBindGroup,
                        shadows.receiverBindGroup(),
                        gpuOcclusion.commandBuffer(),
                        gpuOcclusion.groupFirst().data(),
                        gpuOcclusion.groupCount().data());
                } else {
                    vegetation.draw(frame.cmd, frameBindGroup,
                                    shadows.receiverBindGroup(),
                                    render::VegetationSystem::kVariantCount,
                                    camera.position,
                                    /*forceLowDetail=*/false, &viewFrustum,
                                    occludedSet);
                }
            }
            if (cfg.grass) {
                render::GpuProbe::Scope sub { subProbe, subDevice,
                                              "mainGrass" };
                grass.draw(frame.cmd, frameBindGroup,
                           shadows.receiverBindGroup(), camera.position,
                           &viewFrustum);
            }
        }
        drawSceneMeshes(frame, snapshot, view); // RenderSnapshot.meshes
        drawSkinned(frame, snapshot);        // the Forms-driven skinned NPCs
        if (cfg.sky && !view.interiorMode) {
            sky.draw(frame.cmd, frameBindGroup); // background only
        }
        // Placed water surfaces (alpha) after every opaque.
        if (cfg.water) {
            drawWaterVolumes(frame, snapshot);
        }
        // The frame's particles (camera-facing quads; the
        // extract sorted the alpha batch, additive is order-free).
        fx.draw(frame, *shaders, frameBindGroup, snapshot.fxAlpha,
                snapshot.fxAdditive);
        // Rain streaks (procedural, camera cylinder).
        if (cfg.sky && frameData.stormInfo.y > 0.003f) {
            if (shaders->generation("rain") != rainShaderGeneration ||
                rainPipeline.id() == 0) {
                rainPipeline = { frame.device, frame.device.createPipeline(
                    { .shader = shaders->get("rain"),
                      .blend = rhi::BlendMode::Alpha,
                      .depth = { .testEnable = true,
                                 .writeEnable = false,
                                 .compare = rhi::CompareFunc::Greater }, // reversed-Z
                      .cull = rhi::CullMode::None }) };
                rainShaderGeneration = shaders->generation("rain");
            }
            frame.cmd.setPipeline(rainPipeline);
            frame.cmd.setBindGroup(0, frameBindGroup);
            frame.cmd.setBindGroup(1, rainReceiverGroup);
            frame.cmd.draw(3000 * 6);
        }
        frame.cmd.endRenderPass();
    }

    // Snapshot the opaque scene (sampling a bound attachment is UB): the
    // SSAO pass reads the depth copy EVERY frame — interiors included
    // (skipping it left the previous exterior's AO ghosting over the
    // room). Water composition and Hi-Z occlusion stay exterior-only.
    if (useOffscreen && frame.device.caps().copyTexture &&
        sceneColorCopy.id() != 0) {
        core::FrameProbe::Scope probe { *view.probe, "copyHizWater" };
        render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                      "copyHizWater" };
        frame.cmd.copyTexture(offscreenColor, sceneColorCopy);
        frame.cmd.copyTexture(offscreenDepth, sceneDepthCopy);

        // GPU Hi-Z occlusion: pyramid from this frame's depth
        // snapshot + cull dispatch; the verdict is read back NEXT frame
        // (CPU path) or consumed as indirect commands (GPU-driven path).
        occlusionCommandsFresh = false;
        if (cfg.occlusion && !view.interiorMode &&
            frame.device.caps().computeShaders) {
            gpuOcclusion.resize(frame.device, frame.width, frame.height);
            terrain.collectChunkAabbs(occlusionAabbs);
            occlusionCandidates.clear();
            occlusionCandidates.reserve(occlusionAabbs.size());
            for (const auto& aabb : occlusionAabbs) {
                occlusionCandidates.push_back(
                    { aabb.lo,
                      { aabb.hi.x,
                        aabb.hi.y + render::ChunkOcclusion::kPropHeadroom,
                        aabb.hi.z },
                      aabb.group, aabb.indexCount, aabb.vertexOffset });
            }
            // Vegetation entries (groups 4+): one per chunk×variant, with
            // the level picked now and consumed next frame (I5). Their
            // bigger padded AABBs only ever ADD readback-verdict keys the
            // terrain entries already imply.
            if (cfg.vegetation) {
                vegetation.collectDrawCandidates(occlusionCandidates,
                                                 camera.position);
            }
            // A candidate without an indirect command never draws, so
            // the list must NEVER be truncated (horizon holes) — the cap
            // is sized for the worst case and a clip falls back to the
            // full CPU path (everything draws, just uncull-ed). Loudly:
            // this is a sizing bug, not a mode.
            if (occlusionCandidates.size() > GpuOcclusion::kMaxCandidates) {
                LOG_WARN("GpuOcclusion: {} candidates clip the {} cap — "
                         "CPU fallback this frame",
                         occlusionCandidates.size(),
                         GpuOcclusion::kMaxCandidates);
            }
            occlusionCommandsFresh =
                gpuOcclusion.run(frame.cmd, frame.device, sceneDepthCopy,
                                 viewProj, occlusionCandidates);
        }

        if (cfg.water && !view.interiorMode &&
            waterSceneBindGroup.id() != 0) {
            frame.cmd.beginRenderPass({ .framebuffer = offscreenFb,
                                        .loadOp = rhi::LoadOp::Load,
                                        .depthLoadOp = rhi::LoadOp::Load });
            water.draw(frame.cmd, frameBindGroup, waterSceneBindGroup);
            frame.cmd.endRenderPass();
        }
    }

    // Bloom pyramid + god rays + volumetric shafts, composed by the tonemap.
    if (cfg.postFx && useOffscreen) {
        core::FrameProbe::Scope probe { *view.probe, "postfx" };
        // Volumetric sky clouds FIRST: the god-ray march inside
        // postFx.render composites them (transmittance carves the rays).
        {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "skyclouds" };
            // One-shot noise bake on the FIRST frame (not the first
            // cloudy one): a weather change must never pay it mid-play.
            noiseVolume.bakeIfNeeded(frame.cmd);
            if (frameData.cloudVolInfo.x > 0.5f) {
                postFx.renderSkyClouds(frame.device, frame.cmd, frameData,
                                       frameBindGroup,
                                       noiseVolume.bindGroup(),
                                       sky.cloudMapBindGroup());
            } else {
                postFx.clearSkyClouds(frame.cmd);
            }
        }
        postFx.render(frame.device, frame.cmd, frameData, frameBindGroup,
                      shadows.receiverBindGroup(),
                      radianceCascades.applyGroup(),
                      view.atmos.godRayIntensity > 0.003f, &gpuProbe,
                      sky.cloudMapBindGroup());
        // SSDM scatter (ssdm_*.frag, Lobel 2008): fresh color copy (the
        // pre-water one was consumed), then flow -> bounds quadtree ->
        // nearest-wins resolve back into the offscreen target. Crests
        // extrude over their neighbors (sky included); holes fall back
        // to the gather (pits keep digging).
        if (ssdmModeUi != 0 && useOffscreen &&
            frame.device.caps().copyTexture &&
            ssdmResolvePipeline.get().id != 0 &&
            sceneColorCopy.id() != 0) {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "ssdm" };
            frame.cmd.copyTexture(offscreenColor, sceneColorCopy);
            const auto fullscreen = [&](rhi::FramebufferHandle fb,
                                        rhi::PipelineHandle pipeline,
                                        rhi::BindGroupHandle group) {
                frame.cmd.beginRenderPass(
                    { .framebuffer = fb,
                      .loadOp = rhi::LoadOp::DontCare,
                      .depthLoadOp = rhi::LoadOp::DontCare });
                frame.cmd.setPipeline(pipeline);
                frame.cmd.setBindGroup(0, frameBindGroup);
                frame.cmd.setBindGroup(1, group);
                frame.cmd.draw(3);
                frame.cmd.endRenderPass();
            };
            fullscreen(ssdmFlowFb.get(), ssdmFlowPipeline.get(),
                       ssdmFlowGroup.get());
            fullscreen(ssdmBoundsFb[0].get(), ssdmBounds0Pipeline.get(),
                       ssdmBoundsGroup[0].get());
            for (u32 i = 1; i < kSsdmLevels; ++i) {
                fullscreen(ssdmBoundsFb[i].get(), ssdmDownPipeline.get(),
                           ssdmBoundsGroup[i].get());
            }
            if (ssdmModeUi == 1 && ssdmHalfFb.id() != 0) {
                // Half mode: resolve at chain res into the intermediate
                // (alpha = "this pixel moved"), then the edge-aware
                // upsample rewrites ONLY those pixels at full res.
                frame.cmd.beginRenderPass(
                    { .framebuffer = ssdmHalfFb.get(),
                      .loadOp = rhi::LoadOp::DontCare,
                      .depthLoadOp = rhi::LoadOp::DontCare });
                frame.cmd.setPipeline(ssdmResolveHalfPipeline);
                frame.cmd.setBindGroup(0, frameBindGroup);
                frame.cmd.setBindGroup(1, ssdmResolveGroup);
                frame.cmd.draw(3);
                frame.cmd.endRenderPass();
                frame.cmd.beginRenderPass(
                    { .framebuffer = offscreenFb,
                      .loadOp = rhi::LoadOp::Load,
                      .depthLoadOp = rhi::LoadOp::Load });
                frame.cmd.setPipeline(ssdmUpsamplePipeline);
                frame.cmd.setBindGroup(0, frameBindGroup);
                frame.cmd.setBindGroup(1, ssdmUpsampleGroup.get());
                frame.cmd.draw(3);
                frame.cmd.endRenderPass();
            } else {
                frame.cmd.beginRenderPass(
                    { .framebuffer = offscreenFb,
                      .loadOp = rhi::LoadOp::Load,
                      .depthLoadOp = rhi::LoadOp::Load });
                frame.cmd.setPipeline(ssdmResolvePipeline);
                frame.cmd.setBindGroup(0, frameBindGroup);
                frame.cmd.setBindGroup(1, ssdmResolveGroup);
                frame.cmd.draw(3);
                frame.cmd.endRenderPass();
            }
        }
        // Contact shadows (the texture is the toggle — white = off).
        {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "contact" };
            if (contactShadowsUi && !view.interiorMode) {
                postFx.renderContactShadows(frame.cmd, frameBindGroup,
                                            shadows.receiverBindGroup());
            } else {
                postFx.clearContactShadows(frame.cmd);
            }
        }
        // SSAO (same texture-is-the-toggle contract; sun-independent,
        // so interiors keep it).
        {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "ssao" };
            if (ssaoUi) {
                postFx.renderSsao(frame.cmd, frameBindGroup);
            } else {
                postFx.clearSsao(frame.cmd);
            }
        }
        // Ground mist (the texture is the toggle — neutral = off;
        // frameData.mistInfo.x already folds the composer's gates).
        {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device, "mist" };
            if (frameData.mistInfo.x > 0.0f && mistMap.ready()) {
                postFx.renderMist(frame.device, frame.cmd, frameData,
                                  frameBindGroup,
                                  shadows.receiverBindGroup(),
                                  radianceCascades.applyGroup(),
                                  sky.cloudMapBindGroup(),
                                  mistMap.bindGroup(),
                                  noiseVolume.bindGroup());
            } else {
                postFx.clearMist(frame.cmd);
            }
        }
        // Auto exposure: measure + adapt, before the tonemap taps it.
        if (autoExposureUi) {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "autoExpo" };
            postFx.renderAutoExposure(frame.device, frame.cmd,
                                      frameBindGroup);
        }
    }

    // Pipelined GI (docs/RENDERING.md PG2): the chain records HERE, at the
    // end of the frame — this frame consumed LAST frame's cascade 0; the
    // chain overlaps the composite, the present and the next frame's
    // front, up to the consumer fence before the next reflection.
    if (cfg.gi && radianceCascades.tuning.pipelined) {
        recordGiUpdate(frame, snapshot, view, uniforms, clustered);
    }

    if (useOffscreen) {
        core::FrameProbe::Scope probe { *view.probe, "composite" };
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "composite" };
        // Tonemap composite: HDR scene -> filmic curve -> gamma -> backbuffer.
        frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::DontCare });
        frame.cmd.setPipeline(blitPipeline);
        frame.cmd.setBindGroup(0, frameBindGroup); // FrameUbo (uPostInfo)
        // The side the adaptation pass just wrote.
        frame.cmd.setBindGroup(1, blitBindGroups[postFx.exposureSide()]);
        frame.cmd.draw(3);
        // GI debug raymarch of a clip volume over the frame.
        if (cfg.gi) {
            radianceCascades.drawDebug(frame.cmd, frameBindGroup);
        }
        // The game UI composes over the tonemapped scene,
        // under the dev ImGui layer.
        if (view.gameUi) {
            view.gameUi->resize(frame.width, frame.height); // no-op if same
            view.gameUi->render(frame.cmd, frame.device, frame.width,
                                frame.height);
        }
        frame.cmd.endRenderPass();
    } else if (view.gameUi) {
        frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Load,
                                    .depthLoadOp = rhi::LoadOp::DontCare });
        view.gameUi->resize(frame.width, frame.height);
        view.gameUi->render(frame.cmd, frame.device, frame.width,
                            frame.height);
        frame.cmd.endRenderPass();
    }
}

// The renderer's own dev panels (the tuning state
// they bind lives HERE — the scene keeps the section headers/F-keys).
// The budget table: per-pass GPU average/max over the
// rolling window, with the CPU FrameProbe column beside it (names match
// where both sides instrument the same block). This table IS the
// baseline the optimization bricks are ordered by (docs/RENDERING.md).

// The GI chain's per-frame recording (docs/RENDERING.md):
// gathers the injection lists (props, NPCs, vegetation, lights) and
// records inject/build/extend/merge. Called from render() at its
// historical post-CSM slot, or at the END of the frame when the GI is
// pipelined (docs/RENDERING.md PG2).
void WorldRenderer::recordGiUpdate(engine::FrameContext& frame,
                                       const render::RenderSnapshot& snapshot,
                                       const RenderView& view,
                                       const render::FrameUniforms& uniforms,
                                       bool clustered) {
    const render::Camera3D& camera = view.camera;
        // Props/kits as world AABBs (v1 box occlusion — assumed
        // stylized), NPCs as capsuloid boxes, the frame's local lights.
        rcBoxes.clear();
        rcLights.clear();
        const f32 clipHalf = static_cast<f32>(radianceCascades.tuning
                                                  .resolution) *
                             radianceCascades.tuning.coarseVoxel * 0.5f;
        const Vec3 clipMin = camera.position - Vec3 { clipHalf };
        const Vec3 clipMax = camera.position + Vec3 { clipHalf };
        for (const auto& mesh : snapshot.meshes) {
            if (rcBoxes.size() >= render::RadianceCascades::kMaxBoxes) {
                break;
            }
            if (!mesh.giOccluder) {
                continue; // viewmodel: no self-shadowing GI box
            }
            const render::MeshCache::CpuMesh* cpu =
                view.meshCache ? view.meshCache->cpuMesh(mesh.model)
                               : nullptr;
            if (!cpu) {
                continue; // not resident yet — next inject picks it up
            }
            Vec3 lo { 1e9f };
            Vec3 hi { -1e9f };
            for (u32 corner = 0; corner < 8; ++corner) {
                const Vec3 local {
                    (corner & 1) ? cpu->boundsMax.x : cpu->boundsMin.x,
                    (corner & 2) ? cpu->boundsMax.y : cpu->boundsMin.y,
                    (corner & 4) ? cpu->boundsMax.z : cpu->boundsMin.z
                };
                const Vec3 world =
                    Vec3(mesh.transform * Vec4 { local, 1.0f });
                lo = glm::min(lo, world);
                hi = glm::max(hi, world);
            }
            if (glm::any(glm::lessThan(hi, clipMin)) ||
                glm::any(glm::greaterThan(lo, clipMax))) {
                continue; // outside the coarse clip
            }
            // Albedo proxy: the material tint over a mid gray (real splat
            // colors never leave the mesh shader); emissive feeds the GI.
            rcBoxes.push_back(
                { { lo, 1.0f },
                  { hi, 0.0f },
                  { Vec3 { mesh.tint } * 0.35f, mesh.emissive } });
        }
        for (const auto& npc : snapshot.skinned) {
            if (rcBoxes.size() >= render::RadianceCascades::kMaxBoxes) {
                break;
            }
            const Vec3 feet { npc.transform[3] };
            rcBoxes.push_back({ { feet - Vec3 { 0.4f, 0.0f, 0.4f }, 1.0f },
                                { feet + Vec3 { 0.4f, 1.8f, 0.4f }, 0.0f },
                                { Vec3 { 0.10f }, 0.0f } });
        }
        // The vegetation joins the GI
        // volume — forests get green bounce and canopy sky occlusion.
        // Trees inject their CANOPY as a semi-transparent green box (the
        // march filters through, soft light under the crowns); rocks and
        // bushes as small solid/soft boxes. Nearest chunks first, capped
        // by the shared box budget.
        if (cfg.vegetation && !view.interiorMode) {
            vegGiProps.clear();
            vegetation.collectGiProps(
                camera.position, clipHalf, vegGiProps,
                render::RadianceCascades::kMaxBoxes - rcBoxes.size());
            for (const auto& prop : vegGiProps) {
                const f32 s = prop.scale;
                const Vec3 p = prop.position;
                switch (prop.kind) {
                case 0: // tree canopy (leaf-green, filters light)
                    // PER-VOXEL opacity: realistic-scale canopies span
                    // tens of voxels, and the march multiplies
                    // (1 - opacity) per voxel — anything high saturates
                    // to a pitch-black lid. 0.12 leaves ~20% of the sky
                    // under a dense crown. hand-tuned.
                    rcBoxes.push_back(
                        { { p + Vec3 { -2.2f * s, 2.2f * s, -2.2f * s },
                            0.12f },
                          { p + Vec3 { 2.2f * s, 5.8f * s, 2.2f * s },
                            0.0f },
                          { Vec3 { 0.055f, 0.115f, 0.032f }, 0.0f } });
                    break;
                case 1: // rock
                    rcBoxes.push_back(
                        { { p + Vec3 { -1.1f * s, 0.0f, -1.1f * s }, 1.0f },
                          { p + Vec3 { 1.1f * s, 1.6f * s, 1.1f * s },
                            0.0f },
                          { Vec3 { 0.17f, 0.16f, 0.15f }, 0.0f } });
                    break;
                default: // bush
                    rcBoxes.push_back(
                        { { p + Vec3 { -0.9f * s, 0.0f, -0.9f * s }, 0.8f },
                          { p + Vec3 { 0.9f * s, 1.2f * s, 0.9f * s },
                            0.0f },
                          { Vec3 { 0.050f, 0.105f, 0.030f }, 0.0f } });
                    break;
                }
            }
        }
        for (const auto& light : snapshot.lights) {
            // H2: sun-linked emitters carry the live sun into the GI
            // field too — same color/gate as the direct path above.
            const f32 sunGate =
                glm::smoothstep(0.05f, 0.20f, shadowSunDirection.y);
            const Vec3 color = light.sunLinked
                                   ? Vec3 { uniforms.sunColor }
                                   : light.color;
            f32 intensity = light.intensity;
            if (light.sunLinked) {
                intensity *= sunGate *
                             glm::smoothstep(
                                 0.15f, 0.40f,
                                 glm::dot(-shadowSunDirection,
                                          glm::normalize(light.direction))) *
                             aboveBuried(light.position.y, view.buriedBelowY);
            }
            // §5.1 re-contract: clustered direct reaches EVERY surface,
            // so a normal light's splat drops to its bounce share —
            // full splat would light the ground twice. rcOnly lights
            // keep it all (the field is their lighting). EXTERIOR only:
            // interiors draw no terrain/grass/trees, so clustered adds
            // no new direct receiver there — the tuned interior look
            // keeps its full splat.
            if (clustered && !light.rcOnly && !view.interiorMode) {
                intensity *= radianceCascades.tuning.lightSplatBounce;
            }
            rcLights.push_back({ { light.position, light.radius },
                                 { color * intensity, 0.0f } });
        }
        radianceCascades.update(frame.device, frame.cmd, terrain.params,
                                camera.position, rcBoxes, rcLights,
                                /*bakeTerrain=*/!view.interiorMode,
                                frameBindGroup,
                                shadows.receiverBindGroup(),
                                terrainLightMap.bindGroup(), &frame.device,
                                &gpuProbe);
}

} // namespace render
