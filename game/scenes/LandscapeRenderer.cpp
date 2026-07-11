#include "game/scenes/LandscapeRenderer.hpp"

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
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/ui/UiSystem.hpp"
#include "game/FrameComposer.hpp"
#include "game/MeshCache.hpp"
#include "game/TextureCache.hpp"
#include "game/scenes/LandscapeTuning.hpp"

#include <imgui.h>

namespace game {

namespace {

constexpr const char* kTonemapShader = "tonemap";

// std140 ModelUbo mirror (model + tint + info), shared by the mesh and
// skinned draws — it was re-declared inline in each function body (U4-12).
struct ModelUniforms {
    Mat4 model { 1.0f };
    Vec4 tint { 1.0f };
    Vec4 info { 0.0f }; // x = emissive
};

// std140 LightsUbo mirror (binding 5 — mirrors locallights.glsl).
struct LightsUniforms {
    Vec4 count { 0.0f };
    Vec4 positionRadius[LandscapeRenderer::kMaxLights] {};
    Vec4 colorIntensity[LandscapeRenderer::kMaxLights] {};
    // B1 APPEND: xyz = spot direction, w = cos(half angle); w = -2 marks
    // a point light.
    Vec4 directionAngle[LandscapeRenderer::kMaxLights] {};
};

// Lengyel's oblique near plane: bends the projection's near plane onto an
// arbitrary view-space plane, so the mirrored render clips everything below
// the water for free (no user clip distance in the shaders).
Mat4 obliqueProjection(Mat4 proj, const Vec4& clipPlaneView) {
    Vec4 q;
    q.x = (glm::sign(clipPlaneView.x) + proj[2][0]) / proj[0][0];
    q.y = (glm::sign(clipPlaneView.y) + proj[2][1]) / proj[1][1];
    q.z = -1.0f;
    q.w = (1.0f + proj[2][2]) / proj[3][2];
    const Vec4 c = clipPlaneView * (2.0f / glm::dot(clipPlaneView, q));
    proj[0][2] = c.x;
    proj[1][2] = c.y;
    proj[2][2] = c.z + 1.0f;
    proj[3][2] = c.w;
    return proj;
}

} // namespace

void LandscapeRenderer::applyTuning(
    const data::LandscapeTuningForm& tuning,
    const sptr<const render::HeightPatches>& patches) {
    // Terrain shape + startup values for every live-adjustable knob the
    // render panel owns (§5: the TOML sets where it all starts; the scene
    // keeps the atmosphere half in `atmos`).
    terrain.params.seed = tuning.terrainSeed;
    terrain.params.patches = patches;
    terrain.params.hillWavelength = tuning.hillWavelength;
    terrain.params.hillAmplitude = tuning.hillAmplitude;
    terrain.params.mountainWavelength = tuning.mountainWavelength;
    terrain.params.mountainAmplitude = tuning.mountainAmplitude;
    terrain.params.seaLevel = tuning.seaLevel;
    exposureUi = tuning.exposure;
    // (tuning.ssaoStrength retired 2026-07-10 — screen-space AO removed.)
    gradeVibranceUi = tuning.gradeVibrance;   // B3 (toggle stays off)
    gradeSplitToneUi = tuning.gradeSplitTone;
    gradeContrastUi = tuning.gradeContrast;
    autoExposureMinUi = tuning.autoExposureMin; // B4 (toggle stays off)
    autoExposureMaxUi = tuning.autoExposureMax;
    // GPU-PERF P1: vegetation draw budget (clamped — the streamer ring
    // and the Hi-Z candidate cap size the safe range).
    vegetation.viewRadius = glm::clamp(tuning.vegViewRadius, 4, 15);
    vegetation.highDetailRadius =
        glm::clamp(tuning.vegHighDetailRadius, 0, 8);
}

void LandscapeRenderer::create(rhi::Device& device, core::JobSystem& jobs) {
    frameUbo = { device, device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                     .size = sizeof(render::FrameUniforms),
                                     .dynamic = true },
                                   nullptr) };
    // B5: local lights ride binding 5 of the SAME group — shaders that
    // don't declare the block simply ignore it.
    lightsUbo = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          // B1: + the appended direction/angle array (the UBO lesson:
          // new members go at the END, both CPU and GLSL sides).
          .size = (1 + 3 * kMaxLights) * sizeof(Vec4),
          .dynamic = true },
        nullptr) };
    frameBindGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 0, .buffer = frameUbo },
                       { .binding = 5, .buffer = lightsUbo } } }) };

    shaders = std::make_unique<render::ShaderLibrary>(device);
    terrain.create(device, *shaders, jobs);
    occlusion.create(jobs);
    terrainLightMap.create(device, jobs); // 33b/c
    radianceCascades.create(device, *shaders, jobs); // chantier RC
    grass.create(device, *shaders, jobs);
    vegetation.create(device, *shaders, jobs,
                      terrain.params.seed);

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
    // B2a: the depth-only caster variants (sun cascades).
    shaders->load("shadow_mesh",
                  { { "ShadowUbo", 1 }, { "CasterModelUbo", 4 } });
    shaders->load("shadow_skinned",
                  { { "ShadowUbo", 1 }, { "CasterModelUbo", 4 } });
    buildCasterPipelines(device);
    // Brick 34: dust light shafts.
    shaders->load("lightshaft", { { "FrameUbo", 0 }, { "ShaftUbo", 1 } });
    buildShaftPipeline(device);
    // Brick 32: placed water surfaces.
    shaders->load("watervolume",
                  { { "FrameUbo", 0 }, { "WaterVolumeUbo", 1 } });
    // (Brick 30, cumulonimbus: REMOVED 2026-07-10 — dev call after the
    // first actual display: cost over look. The cloud MAP keeps the sky
    // alive; stormFront still drives rain/wetness via stormInfo.y.)

    // Brick 31: rain — procedural streaks (no buffers) + the top-down
    // occlusion depth so roofs keep the drops out.
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
    rainOcclusionUbo =
        { device, device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                              .size = sizeof(Mat4),
                              .dynamic = true },
                            nullptr) };
    rainCasterGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 1, .buffer = rainOcclusionUbo } } }) };
    rainReceiverGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 9,
                         .texture = rainOcclusionTex,
                         .sampler = rainSampler } } }) };

    // B2b: the interior key-light shadow target (1024², perspective).
    keyShadowTex = { device, device.createTexture(
        { .width = 1024,
          .height = 1024,
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
    keyShadowUbo =
        { device, device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                              .size = sizeof(Mat4),
                              .dynamic = true },
                            nullptr) };
    keyShadowCasterGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 1, .buffer = keyShadowUbo } } }) };
    keyShadowReceiverGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 6,
                         .texture = keyShadowTex,
                         .sampler = keyShadowSampler } } }) };

    shaders->load("skinned",
                  { { "FrameUbo", 0 }, { "ModelUbo", 1 },
                    { "LightsUbo", 5 } },
                  { { "uAlbedo", 0 } });
    // Brick 23: swap one procedural rock variant for an authored CC0 glTF
    // rock (moon_rock_02, Poly Haven). Missing file = procedural fallback.
    if (auto rock = assets::loadGltfMesh(platform::executableDir() / "data" /
                                         "base" / "models" /
                                         "rock_cc0.gltf")) {
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
    sky.create(device, *shaders);
    if (device.caps().offscreenTargets && device.caps().textureArrays) {
        shadows.create(device);
    }
    if (device.caps().copyTexture) {
        water.create(device, *shaders, jobs);
        depthSampler = { device, device.createSampler(
            { .minFilter = rhi::FilterMode::Nearest,
              .magFilter = rhi::FilterMode::Nearest }) };
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
                        // (binding 4 was uSsao — removed 2026-07-10)
                        { "uExposure", 5 },   // B4: adaptation tap
                        { "uContact", 6 } }); // 33a: contact shadows
        rebuildBlitPipeline(device);
    }
    if (device.caps().offscreenTargets && device.caps().hdrFormats &&
        device.caps().copyTexture) {
        postFx.create(device, *shaders);
    }
    if (device.caps().computeShaders && device.caps().copyTexture &&
        device.caps().offscreenTargets) {
        gpuOcclusion.create(device, *shaders);
    }
}

void LandscapeRenderer::destroy(rhi::Device& device) {
    // U4-4: every handle is an rhi::Unique — clearing/resetting frees it
    // through its device; there is no manual destroy mirror to keep in
    // sync anymore. Must run while the device is alive (wrapper contract).
    gpuProbe.shutdown(device); // abandon in-flight timestamps (P0)
    destroyOffscreenTarget(device);
    blitPipeline.reset();
    blitSampler.reset();
    // B1 mesh path: per-entry draw state (the residency caches are
    // scene-owned; their dtors free what they own).
    meshDraws.clear();
    meshPipeline.reset();
    meshCasterPipeline.reset(); // B2a
    skinnedCasterPipeline.reset();
    lightShafts.clear();  // brick 34
    shaftPipeline.reset();
    waterQuads.clear();   // brick 32
    waterVolumePipeline.reset();
    rainPipeline.reset();  // brick 31
    rainReceiverGroup.reset();
    rainCasterGroup.reset();
    rainOcclusionUbo.reset();
    rainOcclusionFb.reset();
    rainSampler.reset();
    rainOcclusionTex.reset();
    keyShadowReceiverGroup.reset(); // B2b
    keyShadowCasterGroup.reset();
    keyShadowUbo.reset();
    keyShadowFb.reset();
    keyShadowSampler.reset();
    keyShadowTex.reset();
    skinnedDraws.clear(); // B6 NPCs (U4-2b)
    skinnedPipeline.reset();
    skinnedShaderGeneration = 0;
    meshSampler.reset();
    whiteTexture.reset();
    gpuOcclusion.destroy(device);
    terrainLightMap.destroy(device); // 33b/c
    radianceCascades.destroy(device); // chantier RC
    postFx.destroy(device);
    water.destroy(device);
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

void LandscapeRenderer::ensureOffscreenTarget(rhi::Device& device, u32 width,
                                           u32 height) {
    if (offscreenFb.id() != 0 && offscreenWidth == width &&
        offscreenHeight == height &&
        appliedReflectionScale == reflectionScaleUi) {
        return;
    }
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
        // GPU-PERF P3: the reflection resolution is a knob (baseline:
        // 1.70 ms at half res) — 0.5 = the historical half-res look,
        // 0.25 = quarter res (blurrier mirror, dev judges).
        const u32 reflectionWidth = glm::max(
            static_cast<u32>(static_cast<f32>(width) * reflectionScaleUi),
            1u);
        const u32 reflectionHeight = glm::max(
            static_cast<u32>(static_cast<f32>(height) * reflectionScaleUi),
            1u);
        appliedReflectionScale = reflectionScaleUi;
        reflectionColor = { device, device.createTexture(
            { .width = reflectionWidth,
              .height = reflectionHeight,
              .format = device.caps().hdrFormats ? rhi::TextureFormat::RGBA16F
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
        waterSceneBindGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = sceneColorCopy,
                             .sampler = blitSampler },
                           { .binding = 1,
                             .texture = sceneDepthCopy,
                             .sampler = depthSampler },
                           { .binding = 2,
                             .texture = reflectionColor,
                             .sampler = blitSampler } } }) };
    }

    if (device.caps().offscreenTargets && device.caps().hdrFormats &&
        device.caps().copyTexture) {
        postFx.resize(device, width, height, offscreenColor, sceneColorCopy,
                      sceneDepthCopy);
    }
    // Tonemap inputs: scene + bloom + god rays (black 1x1 fallbacks are not
    // needed on the 4.6 path — postFx is always ready when we get here).
    // B4: one group per adaptation ping-pong side (binding 5).
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
                            { .binding = 5,
                              .texture = postFx.exposureTexture(side),
                              .sampler = blitSampler },
                            { .binding = 6,
                              .texture = postFx.contactTexture(),
                              .sampler = blitSampler } }
                      : vector<rhi::BindGroupEntry> {
                            { .binding = 0,
                              .texture = offscreenColor,
                              .sampler = blitSampler } } }) };
    }
    offscreenWidth = width;
    offscreenHeight = height;
}

void LandscapeRenderer::destroyOffscreenTarget(rhi::Device& device) {
    (void)device; // U4-4: the Unique wrappers free through their device
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

void LandscapeRenderer::rebuildBlitPipeline(rhi::Device& device) {
    // U4-4: the assignment frees the previous pipeline through the wrapper.
    blitPipeline = { device, device.createPipeline(
                                 { .shader = shaders->get(kTonemapShader) }) };
    blitShaderGeneration = shaders->generation(kTonemapShader);
}

void LandscapeRenderer::drawSceneMeshes(engine::FrameContext& frame,
                                        const RenderSnapshot& snapshot,
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
        const RenderSnapshot::MeshInstance& instance = snapshot.meshes[i];
        const MeshCache::Gpu& mesh = view.meshCache->resolve(instance.model);

        // U4-2a: material fields resolved at extract; only the TEXTURE
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
            // U4-4: the assignment frees the previous group.
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

// --- B5: first-person player -----------------------------------------------------

void LandscapeRenderer::drawSkinned(engine::FrameContext& frame,
                                    const RenderSnapshot& snapshot) {
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
    for (const RenderSnapshot::SkinnedInstance& instance : snapshot.skinned) {
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
            it = skinnedDraws.erase(it); // U4-4: Unique members self-free
        } else {
            ++it;
        }
    }
}

void LandscapeRenderer::buildSkinnedPipeline(rhi::Device& device) {
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
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back }) };
    skinnedShaderGeneration = shaders->generation("skinned");
}

// Bundle the streaming fixups' systems for StreamingController this frame —
// references into the scene plus the focus / fade / mode scalars. Rebuilt each

void LandscapeRenderer::buildShaftPipeline(rhi::Device& device) {
    // Additive, depth-tested against the opaques but never writing —
    // the Skyrim FXShaft blend. Both blade faces show (no cull).
    shaftPipeline = { device, device.createPipeline(
        { .shader = shaders->get("lightshaft"),
          .vertexBuffers =
              { { .stride = 5 * sizeof(f32),
                  .attributes = { { .location = 0,
                                    .format = rhi::VertexFormat::F32x3,
                                    .offset = 0 },
                                  { .location = 1,
                                    .format = rhi::VertexFormat::F32x2,
                                    .offset = 3 * sizeof(f32) } } } },
          .blend = rhi::BlendMode::Additive,
          .depth = { .testEnable = true,
                     .writeEnable = false,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::None }) };
    shaftShaderGeneration = shaders->generation("lightshaft");
}

void LandscapeRenderer::drawLightShafts(engine::FrameContext& frame,
                                        const RenderSnapshot& snapshot,
                                        const RenderView& view,
                                        const Vec3& sunColor) {
    if (!shaftsUi) {
        return;
    }
    if (shaders->generation("lightshaft") != shaftShaderGeneration) {
        buildShaftPipeline(frame.device);
    }
    for (LightShaft& shaft : lightShafts) {
        shaft.seen = false;
    }
    bool any = false;
    for (const ShaftLight& light : snapshot.shafts) { // U4-2a
        // Direction: authored (reference rotation x +Z) or the
        // quantized shadow sun (so window shafts follow the day
        // without re-basing every frame).
        Vec3 dir = light.sunLinked ? -shadowSunDirection : light.direction;
        if (glm::dot(dir, dir) < 1e-6f) {
            continue;
        }
        dir = glm::normalize(dir);
        f32 gate = 1.0f;
        Vec3 color = light.color * light.intensity;
        if (light.sunLinked) {
            gate = glm::smoothstep(0.05f, 0.20f, -dir.y);
            color = sunColor * light.intensity;
        }
        if (view.interiorMode == false && light.sunLinked) {
            // Exterior sun shafts belong to the volumetric pass.
            continue;
        }
        if (gate <= 0.001f) {
            continue;
        }

        LightShaft* slot = nullptr;
        for (LightShaft& shaft : lightShafts) {
            if (shaft.entityId == light.entityId) {
                slot = &shaft;
                break;
            }
        }
        if (!slot) {
            lightShafts.push_back({ light.entityId });
            slot = &lightShafts.back();
        }
        slot->seen = true;

        // Rebuild the blades when the direction moves (sun steps).
        if (slot->vertices.id() == 0 ||
            glm::dot(slot->cachedDir, dir) < 0.99995f) {
            const f32 length = glm::max(light.shaftLength, 0.5f);
            const f32 halfAngle = glm::radians(
                glm::clamp(light.spotAngle > 0.0f ? light.spotAngle : 30.0f,
                           5.0f, 80.0f) *
                0.5f);
            const f32 w0 = 0.08f;
            const f32 w1 = std::tan(halfAngle) * length;
            const Vec3 up = std::abs(dir.y) > 0.95f
                                ? Vec3 { 1.0f, 0.0f, 0.0f }
                                : Vec3 { 0.0f, 1.0f, 0.0f };
            const Vec3 s0 = glm::normalize(glm::cross(dir, up));
            const Vec3 apex = light.position;
            const Vec3 end = apex + dir * length;
            f32 verts[3 * 6 * 5]; // 3 blades x 2 tris x 3 verts x 5f
            u32 cursor = 0;
            const auto push = [&](const Vec3& p, f32 u, f32 v) {
                verts[cursor++] = p.x;
                verts[cursor++] = p.y;
                verts[cursor++] = p.z;
                verts[cursor++] = u;
                verts[cursor++] = v;
            };
            for (u32 blade = 0; blade < 3; ++blade) {
                const f32 angle =
                    static_cast<f32>(blade) * glm::radians(60.0f);
                const Vec3 side =
                    glm::normalize(glm::angleAxis(angle, dir) * s0);
                const Vec3 a0 = apex - side * w0;
                const Vec3 a1 = apex + side * w0;
                const Vec3 b0 = end - side * w1;
                const Vec3 b1 = end + side * w1;
                push(a0, -1.0f, 0.0f);
                push(a1, 1.0f, 0.0f);
                push(b1, 1.0f, 1.0f);
                push(a0, -1.0f, 0.0f);
                push(b1, 1.0f, 1.0f);
                push(b0, -1.0f, 1.0f);
            }
            if (slot->vertices.id() == 0) {
                slot->vertices = { frame.device, frame.device.createBuffer(
                    { .usage = rhi::BufferUsage::Vertex,
                      .size = sizeof(verts),
                      .dynamic = true },
                    verts) };
            } else {
                frame.device.updateBuffer(slot->vertices, verts,
                                          sizeof(verts), 0);
            }
            slot->vertexCount = 18;
            slot->cachedDir = dir;
        }
        if (slot->ubo.id() == 0) {
            slot->ubo = { frame.device, frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  .size = 2 * sizeof(Vec4),
                  .dynamic = true },
                nullptr) };
            slot->group = { frame.device, frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = slot->ubo } } }) };
        }
        const Vec4 uniforms[2] = {
            { color * gate, light.shaftSoftness },
            { light.dustDensity, light.shaftLength, 0.0f, 0.0f }
        };
        frame.device.updateBuffer(slot->ubo, uniforms, sizeof(uniforms), 0);
        if (!any) {
            frame.cmd.setPipeline(shaftPipeline);
            frame.cmd.setBindGroup(0, frameBindGroup);
            any = true;
        }
        frame.cmd.setBindGroup(1, slot->group);
        frame.cmd.setVertexBuffer(0, slot->vertices);
        frame.cmd.draw(slot->vertexCount);
    }
    // Sweep shafts whose entity unloaded with its cell.
    for (auto it = lightShafts.begin(); it != lightShafts.end();) {
        if (!it->seen) {
            it = lightShafts.erase(it); // U4-4: Unique members self-free
        } else {
            ++it;
        }
    }
}

f32 LandscapeRenderer::effectiveWaterSurfaceY(const RenderSnapshot& snapshot,
                                              const RenderView& view) const {
    // Brick 32: the water surface the CAMERA sits under, if any — sea
    // level outdoors, a volume's top when inside one (any worldspace),
    // "dry" otherwise. Feeds the tonemap submersion.
    f32 surface = view.interiorMode ? -1.0e6f : terrain.params.seaLevel;
    const Vec3 eye = view.camera.position;
    for (const WaterVolumeInstance& volume : snapshot.waterVolumes) { // U4-2a
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

void LandscapeRenderer::drawWaterVolumes(engine::FrameContext& frame,
                                         const RenderSnapshot& snapshot) {
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
                         .compare = rhi::CompareFunc::Less },
              .cull = rhi::CullMode::None }) };
        waterVolumeShaderGeneration = shaders->generation("watervolume");
    }
    for (WaterQuad& quad : waterQuads) {
        quad.seen = false;
    }
    bool any = false;
    for (const WaterVolumeInstance& volume : snapshot.waterVolumes) { // U4-2a
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
            it = waterQuads.erase(it); // U4-4: Unique members self-free
        } else {
            ++it;
        }
    }
}

void LandscapeRenderer::buildCasterPipelines(rhi::Device& device) {
    // Position-only attributes over the FULL vertex strides (same buffers
    // as the lit pass); depth state mirrors terrain/vegetation casters.
    meshCasterPipeline = { device, device.createPipeline(
        { .shader = shaders->get("shadow_mesh"),
          .vertexBuffers = { render::meshVertexPositionLayout() }, // U3-5
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

void LandscapeRenderer::drawShadowCasters(engine::FrameContext& frame,
                                          const RenderSnapshot& snapshot,
                                          const RenderView& view,
                                          u32 cascade) {
    drawCastersInto(frame, snapshot, view, shadows.casterBindGroup(cascade),
                    cascade == 0);
}

void LandscapeRenderer::drawCastersInto(engine::FrameContext& frame,
                                        const RenderSnapshot& snapshot,
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
            const RenderSnapshot::MeshInstance& instance = snapshot.meshes[i];
            const MeshCache::Gpu& mesh = view.meshCache->resolve(instance.model);
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
    // 2048px cascade resolution. U4-2b: draws from the snapshot; a
    // first-frame NPC has no draw state yet and simply skips one shadow.
    if (!snapshot.skinned.empty()) {
        frame.cmd.setPipeline(skinnedCasterPipeline);
        frame.cmd.setBindGroup(1, casterGroup);
        for (const RenderSnapshot::SkinnedInstance& instance :
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

void LandscapeRenderer::buildMeshPipeline(rhi::Device& device) {
    meshPipeline = { device, device.createPipeline(
        { .shader = shaders->get("mesh"),
          .vertexBuffers = { render::meshVertexLayout() }, // U3-5
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back }) };
    meshShaderGeneration = shaders->generation("mesh");
}

void LandscapeRenderer::render(engine::FrameContext& frame,
                               const RenderSnapshot& snapshot,
                               const RenderView& view) {
    // GPU-PERF P0: resolve last frames' timestamps (never blocking) and
    // open this frame's slot — the scopes below feed the budget table.
    gpuProbe.beginFrame(frame.device);
    shaders->pollHotReload(frame.dt);
    terrain.refreshPipeline(frame.device, *shaders);
    grass.refreshPipeline(frame.device, *shaders);
    vegetation.refreshPipeline(frame.device, *shaders);
    sky.refreshPipeline(frame.device, *shaders);
    if (frame.device.caps().copyTexture) {
        water.refreshPipeline(frame.device, *shaders);
    }
    postFx.refreshPipelines(frame.device, *shaders);
    gpuOcclusion.refreshPipelines(frame.device, *shaders);
    radianceCascades.refreshPipelines(frame.device, *shaders);
    terrain.setWireframe(wireframeUi, frame.device, *shaders);
    if (regenerateRequested) {
        regenerateRequested = false;
        terrain.regenerate(frame.device);
        grass.regenerate(frame.device);
        vegetation.regenerate(frame.device, terrain.params.seed);
        occlusion.invalidate();
    }
    // Grass panel: a scatter knob moved — re-scatter the meadow only.
    if (grassRescatterRequested) {
        grassRescatterRequested = false;
        grass.regenerate(frame.device);
    }
    // Terrain sculpt: re-mesh JUST the chunks a stroke touched (in place, no
    // hole) — runs live during the stroke for real-time feedback. Grass/veg
    // re-scatter only on commit (`sculptScatterChunks`) so they don't flicker
    // every preview frame. The rest of the world stays put.
    if (!sculptDirtyChunks.empty()) {
        terrain.remeshChunks(sculptDirtyChunks);
        sculptDirtyChunks.clear();
    }
    if (!sculptScatterChunks.empty()) {
        grass.invalidateChunks(frame.device, sculptScatterChunks);
        vegetation.invalidateChunks(frame.device, sculptScatterChunks);
        sculptScatterChunks.clear();
    }
    if (!view.interiorMode) { // interiors: no terrain/scatter/water to stream
        {
            core::FrameProbe::Scope probe { *view.probe, "terrain" };
            terrain.update(frame.device, view.camera.position);
        }
        {
            // P1: probed apart from the chunk streaming — a landing bake
            // recreates the 256² map (upload) and must be attributable.
            core::FrameProbe::Scope probe { *view.probe, "lightmap" };
            // 33b/c: pump/kick the light-map bake (worker; re-bakes on
            // the quantized sun step or when the focus strays).
            terrainLightMap.update(frame.device, terrain.params,
                                   view.camera.position,
                                   shadowSunDirection);
        }
        // Height-horizon occlusion (brick 26): rebuilt on a worker
        // whenever the camera strays; stays valid (conservative) meanwhile.
        {
            core::FrameProbe::Scope probe { *view.probe, "occlusion" };
            occlusion.pump();
            if (occlusion.wantsRebuild(view.camera.position)) {
                occlusion.rebuild(terrain.params, view.camera.position,
                                  terrain.chunkTops());
            }
        }
        {
            core::FrameProbe::Scope probe { *view.probe, "grass" };
            grass.update(frame.device, terrain.params,
                         view.camera.position);
        }
        {
            core::FrameProbe::Scope probe { *view.probe, "veg" };
            vegetation.update(frame.device, terrain.params,
                              view.camera.position);
        }
        if (frame.device.caps().copyTexture) {
            core::FrameProbe::Scope probe { *view.probe, "water" };
            water.update(frame.device, terrain.params,
                         view.camera.position);
        }
    }

    const render::Camera3D& camera = view.camera;
    const Mat4 viewProj = camera.viewProj(frame.aspect);
    // CPU chunk culling (brick 25): one frustum per rendered viewpoint.
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
    // The cascades use a QUANTIZED sun (dev report: tree shadows tremble).
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
    // GPU-PERF P5c: which cascades actually re-render this frame. The
    // baseline showed `shadows` at 5.5 ms — over half of it is the two
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
    // and (P3 auto-skip) only when some water can actually show: a
    // RESIDENT chunk dipping below sea level inside the frustum. The
    // known miss: sea at the horizon beyond the resident ring (~960 m) —
    // the A/B toggle is there for that exact check.
    bool waterVisible = true;
    if (reflectionAutoSkipUi && !view.interiorMode && reflectionsUi) {
        waterVisible = false;
        terrain.collectChunkAabbs(occlusionAabbs);
        const f32 sea = terrain.params.seaLevel;
        for (const auto& aabb : occlusionAabbs) {
            if (aabb.lo.y >= sea) {
                continue; // fully above the water table
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

    // Chantier RC: decide this frame's inject + snap the grid origins
    // BEFORE the UBO composition so uGiGridInfo matches the volume
    // content the apply will sample (the moving-halo lag fix).
    radianceCascades.prepare(camera.position);

    // The whole UBO composition is pure (audit U4-6a): gather the inputs,
    // let the composer build both variants, upload. This side keeps only
    // the GPU-availability gates and the updateBuffer calls.
    const ComposedFrame composed = composeFrameUniforms({
        .viewProj = viewProj,
        .cameraPosition = camera.position,
        .width = frame.width,
        .height = frame.height,
        .dt = frame.dt,
        .timeSeconds = view.timeSeconds,
        .sky = skyState,
        .cascades = cascades,
        .shadowStrength = shadowStrength,
        .atmos = view.atmos,
        .interiorMode = view.interiorMode,
        .interiorAmbient = view.interiorAmbient,
        .seaLevel = terrain.params.seaLevel,
        .snowLine = view.snowLine,
        .splatUvScale = view.splatUvScale,
        .reflectionsActive = reflectionsActive,
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
        .grassBaseColor = { grass.renderTuning.baseColor,
                            grass.renderTuning.fadeStart },
        .grassTipColor = { grass.renderTuning.tipColor,
                           grass.renderTuning.fadeEnd },
        // Chantier RC G6 (+ interiors, dev feedback 2026-07-11):
        // giInfo() gates on ready() itself.
        .giInfo = radianceCascades.giInfo(),
        .giGridInfo = radianceCascades.giGridInfo(),
    });
    const render::FrameUniforms& uniforms = composed.base;
    render::FrameUniforms frameData = composed.resolved;
    if (frameData.stormInfo.y > 0.003f) {
        frame.device.updateBuffer(rainOcclusionUbo,
                                  &frameData.rainOcclusionViewProj,
                                  sizeof(Mat4), 0);
    }
    frame.device.updateBuffer(frameUbo, &frameData, sizeof(frameData), 0);

    // B5: the 16 nearest local lights, flicker applied CPU-side (sin +
    // per-index phase — cheap and stateless).
    {
        LightsUniforms lights;
        const vector<SceneLight>& nearest = snapshot.lights; // U4-2a
        lights.count.x = static_cast<f32>(nearest.size());
        for (u32 i = 0; i < nearest.size(); ++i) {
            const SceneLight& light = nearest[i];
            f32 intensity = light.intensity;
            if (light.flicker > 0.0f) {
                const f32 phase = static_cast<f32>(i) * 1.7f;
                intensity *=
                    1.0f + light.flicker *
                               (0.55f * std::sin(view.timeSeconds * 9.0f + phase) +
                                0.45f * std::sin(view.timeSeconds * 23.0f +
                                                 phase * 3.1f));
            }
            lights.positionRadius[i] = { light.position, light.radius };
            lights.colorIntensity[i] = { light.color * intensity, 0.0f };
            const bool spot = light.spotAngle > 0.0f;
            lights.directionAngle[i] = {
                glm::normalize(light.direction),
                spot ? std::cos(glm::radians(light.spotAngle * 0.5f))
                     : -2.0f
            };
        }
        frame.device.updateBuffer(lightsUbo, &lights, sizeof(lights), 0);
    }

    // Bake this frame's cloud field before anything lights with it.
    if (!view.interiorMode) {
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "cloudBake" };
        sky.bakeCloudMap(frame.cmd, frameBindGroup);
    }

    // B2b — the interior key-light shadow: pick the castsShadow light
    // nearest the camera, render its perspective depth, and hand the
    // matrix + position to locallights.glsl (matched by position there).
    bool keyShadowActive = false;
    if (keyShadowUi && view.interiorMode && meshShadowCastersUi) {
        f32 bestDistSq = 1e12f;
        Vec3 keyPos {};
        Vec3 keyDir { 0.0f, 0.0f, 1.0f };
        f32 keyFov = 100.0f;
        f32 keyRadius = 10.0f;
        for (const SceneLight& light : snapshot.shadowLights) { // U4-2a
            const Vec3 d = light.position - camera.position;
            const f32 distSq = glm::dot(d, d);
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                keyPos = light.position;
                keyDir = light.direction;
                keyFov = light.spotAngle > 0.0f
                             ? glm::min(light.spotAngle * 1.3f, 150.0f)
                             : 120.0f;
                keyRadius = light.radius;
            }
        }
        if (bestDistSq < 1e12f) {
            const Vec3 up = std::abs(keyDir.y) > 0.95f
                                ? Vec3 { 1.0f, 0.0f, 0.0f }
                                : Vec3 { 0.0f, 1.0f, 0.0f };
            const Mat4 lightView = glm::lookAt(keyPos, keyPos + keyDir, up);
            const Mat4 proj = glm::perspective(
                glm::radians(keyFov), 1.0f, 0.05f, keyRadius);
            frameData.keyShadowViewProj = proj * lightView;
            frameData.keyShadowInfo = { keyPos, 1.0f };
            frame.device.updateBuffer(frameUbo, &frameData,
                                      sizeof(frameData), 0);
            frame.device.updateBuffer(keyShadowUbo,
                                      &frameData.keyShadowViewProj,
                                      sizeof(Mat4), 0);
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "keyShadow" };
            frame.cmd.beginRenderPass(
                { .framebuffer = keyShadowFb,
                  .loadOp = rhi::LoadOp::DontCare,
                  .depthLoadOp = rhi::LoadOp::Clear });
            drawCastersInto(frame, snapshot, view, keyShadowCasterGroup,
                            /*refreshUbos=*/true);
            frame.cmd.endRenderPass();
            keyShadowActive = true;
        }
    }
    (void)keyShadowActive;

    // Brick 31: the top-down rain occlusion depth (roof cover).
    if (frameData.stormInfo.y > 0.003f && meshShadowCastersUi) {
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "rainOcc" };
        frame.cmd.beginRenderPass({ .framebuffer = rainOcclusionFb,
                                    .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::Clear });
        drawCastersInto(frame, snapshot, view, rainCasterGroup,
                        /*refreshUbos=*/true);
        frame.cmd.endRenderPass();
    }

    // Cascade passes: depth-only casters from the sun's point of view.
    if (shadowStrength > 0.0f) {
        core::FrameProbe::Scope probe { *view.probe, "shadows" };
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "shadows" };
        for (u32 i = 0; i < render::ShadowMapper::kCascadeCount; ++i) {
            if (!cascadeDue[i]) {
                continue; // P5c: kept last frame's depth AND matrix
            }
            frame.cmd.beginRenderPass(
                { .framebuffer = shadows.framebuffer(i),
                  .loadOp = rhi::LoadOp::DontCare,
                  .depthLoadOp = rhi::LoadOp::Clear });
            terrain.drawDepth(frame.cmd, shadows.casterBindGroup(i),
                              camera.position, 9);
            // Same 9-chunk cap: the last cascade ends at 480 m.
            vegetation.drawDepth(frame.cmd, frameBindGroup,
                                 shadows.casterBindGroup(i),
                                 camera.position, 9);
            // B2a: scene meshes + NPCs join the casters (A/B toggle).
            if (meshShadowCastersUi) {
                drawShadowCasters(frame, snapshot, view, i);
            }
            frame.cmd.endRenderPass();
        }
    }

    // Chantier RC (G2/G3): re-inject the GI voxel clipmap — after the CSM
    // passes (the inject samples fresh shadow maps), outside any render
    // pass (compute). Interiors too (dev feedback 2026-07-11): no terrain
    // there, the kit boxes + local lights carry the room.
    {
        // G3: props/kits as world AABBs (v1 box occlusion — assumed
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
            const MeshCache::CpuMesh* cpu =
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
        // Chantier RC (dev report 2026-07-11): the vegetation joins the
        // volume — forests get green bounce and canopy sky occlusion.
        // Trees inject their CANOPY as a semi-transparent green box (the
        // march filters through, soft light under the crowns); rocks and
        // bushes as small solid/soft boxes. Nearest chunks first, capped
        // by the shared box budget.
        if (!view.interiorMode) {
            vegGiProps.clear();
            vegetation.collectGiProps(
                camera.position, clipHalf, vegGiProps,
                render::RadianceCascades::kMaxBoxes - rcBoxes.size());
            for (const auto& prop : vegGiProps) {
                const f32 s = prop.scale;
                const Vec3 p = prop.position;
                switch (prop.kind) {
                case 0: // tree canopy (leaf-green, filters light)
                    rcBoxes.push_back(
                        { { p + Vec3 { -2.2f * s, 2.2f * s, -2.2f * s },
                            0.7f },
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
            rcLights.push_back(
                { { light.position, light.radius },
                  { light.color * light.intensity, 0.0f } });
        }
        radianceCascades.update(frame.device, frame.cmd, terrain.params,
                                camera.position, rcBoxes, rcLights,
                                /*bakeTerrain=*/!view.interiorMode,
                                frameBindGroup,
                                shadows.receiverBindGroup(),
                                terrainLightMap.bindGroup(), &frame.device,
                                &gpuProbe);
    }

    const bool useOffscreen = frame.device.caps().offscreenTargets;
    if (useOffscreen) {
        ensureOffscreenTarget(frame.device, frame.width, frame.height);
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
            obliqueProjection(camera.proj(frame.aspect), planeView);
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
        frame.device.updateBuffer(reflectionUbo, &reflectionUniforms,
                                  sizeof(reflectionUniforms), 0);

        frame.cmd.beginRenderPass({ .framebuffer = reflectionFb,
                                    .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::Clear });
        frame.cmd.setFrontFace(rhi::FrontFace::Clockwise);
        if (sky.cloudMapBindGroup().id != 0) {
            frame.cmd.setBindGroup(3, sky.cloudMapBindGroup());
        }
        if (terrainLightMap.bindGroup().id != 0) {
            frame.cmd.setBindGroup(4, terrainLightMap.bindGroup()); // 33b/c
        }
        terrain.draw(frame.cmd, reflectionBindGroup,
                     shadows.receiverBindGroup(), &reflectionFrustum);
        // Trees only: rocks and bushes are invisible in a wobbly half-res
        // reflection — low-detail canopies for the same reason.
        vegetation.draw(frame.cmd, reflectionBindGroup,
                        shadows.receiverBindGroup(),
                        render::VegetationSystem::kTreeVariants,
                        camera.position, /*forceLowDetail=*/true,
                        &reflectionFrustum);
        sky.draw(frame.cmd, reflectionBindGroup);
        frame.cmd.endRenderPass();
    }

    // P1: the Hi-Z verdict pickup, probed on its own — this is the former
    // hidden mainPass stall (sync readback). With the fence gate it now
    // costs ~0 and keeps LAST frame's verdict while the GPU is behind
    // (`gpuOccluded` persists; collectResults replaces it only when a
    // fresh verdict is actually ready).
    {
        core::FrameProbe::Scope probe { *view.probe, "hiz" };
        gpuOcclusion.collectResults(frame.device, gpuOccluded);
    }

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
              .depthLoadOp = rhi::LoadOp::Clear });
        if (sky.cloudMapBindGroup().id != 0) {
            frame.cmd.setBindGroup(3, sky.cloudMapBindGroup());
        }
        if (terrainLightMap.bindGroup().id != 0) {
            frame.cmd.setBindGroup(4, terrainLightMap.bindGroup()); // 33b/c
        }
        if (keyShadowReceiverGroup.id() != 0) {
            frame.cmd.setBindGroup(5, keyShadowReceiverGroup); // B2b
        }
        if (radianceCascades.applyGroup().id != 0) {
            // Chantier RC G6: the merged cascade 0 for gi.glsl (unit 11).
            frame.cmd.setBindGroup(6, radianceCascades.applyGroup());
        }
        // Occlusion applies to the main view only: both sets were built for
        // the real camera, not the mirrored one (the grass ring is too
        // close to ever be ridge-occluded — frustum only). CPU ∪ GPU Hi-Z.
        combinedOccluded.clear();
        if (occlusionUi && occlusion.occludedSet()) {
            combinedOccluded = *occlusion.occludedSet();
        }
        if (gpuOcclusionUi) {
            combinedOccluded.insert(gpuOccluded.begin(), gpuOccluded.end());
        }
        const std::unordered_set<u64>* occludedSet =
            combinedOccluded.empty() ? nullptr : &combinedOccluded;
        if (!view.interiorMode) {
            {
                render::GpuProbe::Scope sub { gpuProbe, frame.device,
                                              "mainTerrain" };
                terrain.draw(frame.cmd, frameBindGroup,
                             shadows.receiverBindGroup(), &viewFrustum,
                             occludedSet);
            }
            {
                render::GpuProbe::Scope sub { gpuProbe, frame.device,
                                              "mainVeg" };
                vegetation.draw(frame.cmd, frameBindGroup,
                                shadows.receiverBindGroup(),
                                render::VegetationSystem::kVariantCount,
                                camera.position,
                                /*forceLowDetail=*/false, &viewFrustum,
                                occludedSet);
            }
            {
                render::GpuProbe::Scope sub { gpuProbe, frame.device,
                                              "mainGrass" };
                grass.draw(frame.cmd, frameBindGroup,
                           shadows.receiverBindGroup(), camera.position,
                           &viewFrustum);
            }
        }
        drawSceneMeshes(frame, snapshot, view); // B1: the RenderSnapshot.meshes consumer
        drawSkinned(frame, snapshot);        // B6: the Forms-driven skinned NPCs
        if (!view.interiorMode) {
            sky.draw(frame.cmd, frameBindGroup); // background only
        }
        // Brick 32: placed water surfaces (alpha), then brick 34:
        // additive dust shafts — both after every opaque.
        drawWaterVolumes(frame, snapshot);
        drawLightShafts(frame, snapshot, view, skyState.sunColor);
        // Brick 31: rain streaks (procedural, camera cylinder).
        if (frameData.stormInfo.y > 0.003f) {
            if (shaders->generation("rain") != rainShaderGeneration ||
                rainPipeline.id() == 0) {
                rainPipeline = { frame.device, frame.device.createPipeline(
                    { .shader = shaders->get("rain"),
                      .blend = rhi::BlendMode::Alpha,
                      .depth = { .testEnable = true,
                                 .writeEnable = false,
                                 .compare = rhi::CompareFunc::Less },
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
        waterSceneBindGroup.id() != 0) {
        core::FrameProbe::Scope probe { *view.probe, "copyHizWater" };
        render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                      "copyHizWater" };
        frame.cmd.copyTexture(offscreenColor, sceneColorCopy);
        frame.cmd.copyTexture(offscreenDepth, sceneDepthCopy);

        // GPU Hi-Z occlusion (brick 26): pyramid from this frame's depth
        // snapshot + cull dispatch; the verdict is read back NEXT frame.
        if (!view.interiorMode && frame.device.caps().computeShaders) {
            gpuOcclusion.resize(frame.device, frame.width, frame.height);
            terrain.collectChunkAabbs(occlusionAabbs);
            occlusionCandidates.clear();
            occlusionCandidates.reserve(occlusionAabbs.size());
            for (const auto& aabb : occlusionAabbs) {
                occlusionCandidates.push_back(
                    { aabb.key, aabb.lo,
                      { aabb.hi.x,
                        aabb.hi.y + render::ChunkOcclusion::kPropHeadroom,
                        aabb.hi.z } });
            }
            gpuOcclusion.run(frame.cmd, frame.device, sceneDepthCopy,
                             viewProj, occlusionCandidates);
        }

        if (!view.interiorMode) {
            frame.cmd.beginRenderPass({ .framebuffer = offscreenFb,
                                        .loadOp = rhi::LoadOp::Load,
                                        .depthLoadOp = rhi::LoadOp::Load });
            water.draw(frame.cmd, frameBindGroup, waterSceneBindGroup);
            frame.cmd.endRenderPass();
        }
    }

    // Bloom pyramid + god rays + volumetric shafts, composed by the tonemap.
    // Unit 2 (cloud map) persists across the post passes for the march.
    if (useOffscreen) {
        core::FrameProbe::Scope probe { *view.probe, "postfx" };
        if (sky.cloudMapBindGroup().id != 0) {
            frame.cmd.setBindGroup(3, sky.cloudMapBindGroup());
        }
        postFx.render(frame.cmd, frameBindGroup,
                      shadows.receiverBindGroup(), &frame.device,
                      &gpuProbe);
        // 33a: contact shadows (the texture is the toggle — white = off).
        {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "contact" };
            if (contactShadowsUi && !view.interiorMode) {
                postFx.renderContactShadows(frame.cmd, frameBindGroup);
            } else {
                postFx.clearContactShadows(frame.cmd);
            }
        }
        // B4 (brick 29): measure + adapt, before the tonemap taps it.
        if (autoExposureUi) {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "autoExpo" };
            postFx.renderAutoExposure(frame.device, frame.cmd,
                                      frameBindGroup);
        }
    }

    if (useOffscreen) {
        core::FrameProbe::Scope probe { *view.probe, "composite" };
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "composite" };
        // Tonemap composite: HDR scene -> filmic curve -> gamma -> backbuffer.
        frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::DontCare });
        frame.cmd.setPipeline(blitPipeline);
        frame.cmd.setBindGroup(0, frameBindGroup); // FrameUbo (uPostInfo)
        // B4: the side the adaptation pass just wrote.
        frame.cmd.setBindGroup(1, blitBindGroups[postFx.exposureSide()]);
        frame.cmd.draw(3);
        // Chantier RC: debug raymarch of a clip volume over the frame.
        radianceCascades.drawDebug(frame.cmd, frameBindGroup);
        // Chantier 4: the game UI composes over the tonemapped scene,
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

// The renderer's own dev panels (audit U4-1 leftovers: the tuning state
// they bind lives HERE — the scene keeps the section headers/F-keys).
// GPU-PERF P0 — the budget table: per-pass GPU average/max over the
// rolling window, with the CPU FrameProbe column beside it (names match
// where both sides instrument the same block). This table IS the
// baseline the optimization bricks are ordered by (docs/GPU-PERF.md).
void LandscapeRenderer::drawPerfPanel(const core::FrameProbe* cpuProbe) {
    if (!gpuProbe.active()) {
        ImGui::TextDisabled("(no GPU timer queries on this device)");
        return;
    }
    ImGui::Text("GPU frame: %.2f ms avg  %.2f ms max",
                gpuProbe.frameAverageMs(), gpuProbe.frameMaxMs());
    ImGui::SameLine();
    if (ImGui::SmallButton("reset window")) {
        gpuProbe.resetWindow();
    }
    if (!ImGui::BeginTable("gpuperf", 4,
                           ImGuiTableFlags_SizingStretchProp |
                               ImGuiTableFlags_RowBg)) {
        return;
    }
    ImGui::TableSetupColumn("pass");
    ImGui::TableSetupColumn("GPU avg (ms)");
    ImGui::TableSetupColumn("GPU max");
    ImGui::TableSetupColumn("CPU (ms)");
    ImGui::TableHeadersRow();
    for (const render::GpuProbe::PassRow& row : gpuProbe.rows()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.name);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", row.stats.averageMs);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", row.stats.maxMs);
        ImGui::TableNextColumn();
        // The CPU column: the FrameProbe scope of the SAME name when one
        // exists (the CPU probes also cover streaming blocks the GPU
        // never sees — those rows are simply absent here).
        f64 cpuMs = -1.0;
        if (cpuProbe) {
            for (const core::FrameProbe::Entry& entry :
                 cpuProbe->currentEntries()) {
                if (std::strcmp(entry.name, row.name) == 0) {
                    cpuMs = entry.ms;
                    break;
                }
            }
        }
        if (cpuMs >= 0.0) {
            ImGui::Text("%.2f", cpuMs);
        } else {
            ImGui::TextDisabled("-");
        }
    }
    ImGui::EndTable();
    if (gpuProbe.rows().empty()) {
        ImGui::TextDisabled("(warming up — first frames resolving)");
    }
}

void LandscapeRenderer::drawTerrainPanel() {
    // Live stats stay on top, always visible; the knobs group below.
    ImGui::Text("Resident: %u | drawn: %u | pending: %u | uploads: %u",
                terrain.residentCount(), terrain.drawnLastFrame(),
                terrain.pendingCount(), terrain.uploadsLastFrame());
    ImGui::Text("Prop chunks drawn: %u | occluded CPU: %u | GPU: %u",
                vegetation.drawnLastFrame(), occlusion.occludedCount(),
                gpuOcclusion.lastOccludedCount());
    ImGui::Text("Grass blades: %u | props: %u", grass.instanceTotal(),
                vegetation.propTotal());
    if (ImGui::CollapsingHeader("Terrain",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputScalar("Seed", ImGuiDataType_U32, &terrain.params.seed);
        ImGui::SameLine();
        if (ImGui::Button("Regenerate")) {
            regenerateRequested = true; // applied at the next render
        }
        // Water plane, sand band and material weights follow live; the
        // scatter (grass/trees/props) is baked per chunk — Regenerate to
        // re-align it.
        ImGui::SliderFloat("Sea level (m)", &terrain.params.seaLevel, 0.0f,
                           40.0f, "%.0f");
    }
    if (ImGui::CollapsingHeader("Vegetation")) {
        // GPU-PERF P1: the vegetation draw budget, live (baseline:
        // mainVeg 1.8 ms at 14/4). Shrinking the ring pops at the edge
        // (the tree fade tops out at 880 m) — a budget-hunting knob.
        ImGui::SliderInt("Veg view radius (chunks)", &vegetation.viewRadius,
                         4, 15);
        ImGui::SliderInt("Veg high-detail radius",
                         &vegetation.highDetailRadius, 0, 8);
    }
    if (ImGui::CollapsingHeader("Culling & debug")) {
        ImGui::Checkbox("Occlusion culling (A/B)", &occlusionUi);
        ImGui::SameLine();
        ImGui::Checkbox("GPU Hi-Z", &gpuOcclusionUi);
        ImGui::Checkbox("Wireframe (LOD debug)", &wireframeUi);
    }
}

void LandscapeRenderer::drawRenderPanel(AtmosphereParams& atmos) {
    // Grass redo #2 (2026-07-11): every meadow constant, live. The render
    // half rides the FrameUbo; a scatter knob queues a grass-only
    // re-scatter on release (budgeted — the ring rebuilds over frames).
    if (ImGui::CollapsingHeader("Grass")) {
        render::GrassRenderTuning& gt = grass.renderTuning;
        ImGui::SeparatorText("Blade");
        ImGui::SliderFloat("Height (m)", &gt.bladeHeight, 0.2f, 2.0f,
                           "%.2f");
        ImGui::SliderFloat("Half width (m)", &gt.bladeHalfWidth, 0.01f,
                           0.12f, "%.3f");
        ImGui::ColorEdit3("Base color", &gt.baseColor.x,
                          ImGuiColorEditFlags_Float);
        ImGui::ColorEdit3("Tip color", &gt.tipColor.x,
                          ImGuiColorEditFlags_Float);
        ImGui::SeparatorText("Detail / distance");
        ImGui::SliderFloat("Detail near (m)", &gt.detailNear, 2.0f, 60.0f,
                           "%.0f");
        ImGui::SliderFloat("Detail far (m)", &gt.detailFar, 5.0f, 120.0f,
                           "%.0f");
        ImGui::SliderFloat("Thin start (m)", &gt.thinStart, 2.0f, 100.0f,
                           "%.0f");
        ImGui::SliderFloat("Thin end (m)", &gt.thinEnd, 20.0f, 200.0f,
                           "%.0f");
        ImGui::SliderFloat("Far density", &gt.farDensity, 0.05f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Far width comp", &gt.widthCompensation, 0.0f,
                           3.0f, "%.1f");
        ImGui::SliderFloat("Fade start (m)", &gt.fadeStart, 40.0f, 300.0f,
                           "%.0f");
        ImGui::SliderFloat("Fade end (m)", &gt.fadeEnd, 60.0f, 350.0f,
                           "%.0f");
        ImGui::SeparatorText("Scatter (re-bakes on release)");
        render::GrassScatterTuning& st = grass.scatterTuning;
        bool scatterEdited = false;
        ImGui::SliderFloat("Blade spacing (m)", &st.spacing, 0.08f, 0.5f,
                           "%.2f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Patch scale (m)", &st.patchBroadScale, 4.0f,
                           60.0f, "%.0f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Clump detail (m)", &st.patchDetailScale, 1.0f,
                           20.0f, "%.0f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::DragFloatRange2("Patch threshold", &st.patchThresholdLo,
                               &st.patchThresholdHi, 0.005f, 0.0f, 1.0f,
                               "lo %.2f", "hi %.2f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::DragFloatRange2("Presence window", &st.presenceLo,
                               &st.presenceHi, 0.005f, 0.0f, 1.0f,
                               "rim %.2f", "solid %.2f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Material cutoff", &st.materialCutoff, 0.0f,
                           1.0f, "%.2f");
        scatterEdited |= ImGui::IsItemDeactivatedAfterEdit();
        if (scatterEdited || ImGui::Button("Rescatter now")) {
            grassRescatterRequested = true;
        }
    }
    // Chantier RC: every cost-affecting GI parameter is a live knob (dev
    // workflow: quality first, HE does the perf descent here, watching
    // the rcInject/rcBuild lines of the F6 table).
    if (ImGui::CollapsingHeader("Global illumination")) {
        render::RcTuning& rc = radianceCascades.tuning;
        int technique = rc.technique == render::GiTechnique::RadianceCascades
                            ? 1 : 0;
        if (ImGui::Combo("Technique", &technique,
                         "Classic (ambient x light map)\0"
                         "Radiance cascades (WIP)\0")) {
            rc.technique = technique == 1
                               ? render::GiTechnique::RadianceCascades
                               : render::GiTechnique::Classic;
        }
        ImGui::SeparatorText("Voxel clipmap");
        ImGui::SliderInt("Resolution (voxels)", &rc.resolution, 32, 96);
        ImGui::SliderFloat("Fine voxel (m)", &rc.fineVoxel, 0.25f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Coarse voxel (m)", &rc.coarseVoxel, 1.0f, 4.0f,
                           "%.1f");
        ImGui::SliderInt("Update every N frames", &rc.updateInterval, 1, 4);
        ImGui::SeparatorText("Cascades");
        ImGui::SliderInt("Cascade count", &rc.cascadeCount, 2, 6);
        ImGui::SliderFloat("Interval 0 (m)", &rc.interval0, 0.25f, 4.0f,
                           "%.2f");
        ImGui::TextDisabled("reach: %.0f m",
                            rc.interval0 *
                                (std::pow(2.0f, static_cast<f32>(
                                                    rc.cascadeCount)) -
                                 1.0f));
        ImGui::SeparatorText("Injection");
        ImGui::SliderFloat("Sky factor", &rc.skyFactor, 0.0f, 1.0f, "%.2f");
        ImGui::SeparatorText("Apply");
        ImGui::SliderFloat("Intensity", &rc.intensity, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Edge fade (m)", &rc.edgeFade, 1.0f, 16.0f,
                           "%.0f");
        // Adaptive stylized ramp: flat pools over the MEASURED contrast.
        ImGui::SliderInt("Stylized bands", &rc.bands, 2, 5);
        ImGui::SliderFloat("Contrast floor (stops)", &rc.contrastFloor,
                           0.25f, 3.0f, "%.2f");
        ImGui::SliderFloat("Adapt speed", &rc.adaptSpeed, 0.01f, 0.30f,
                           "%.2f");
        ImGui::Combo("Debug view", &rc.debugView,
                     "Off\0Fine clip (raymarch)\0Coarse clip (raymarch)\0"
                     "Cascade 0 irradiance\0");
        // The GPU cost lines, in place (the full table stays on F6).
        for (const auto& row : gpuProbe.rows()) {
            if (row.name && str(row.name).rfind("rc", 0) == 0) {
                ImGui::TextDisabled("%s: %.2f ms (max %.2f)", row.name,
                                    row.stats.averageMs, row.stats.maxMs);
            }
        }
    }
    if (ImGui::CollapsingHeader("Lighting & shadows")) {
        ImGui::Checkbox("Stylized lighting (BotW A/B)", &stylizedUi);
        ImGui::Checkbox("Shadows", &shadowsUi);
        ImGui::SameLine();
        ImGui::Checkbox("Cascade debug tint", &cascadeDebugUi);
        // GPU-PERF P5c A/B: far cascades on alternate frames (baseline had
        // `shadows` at 5.5 ms) — off = every cascade every frame.
        ImGui::Checkbox("CSM round-robin (far cascades 1/2 rate)",
                        &shadowRoundRobinUi);
        // B2a A/B: houses/crates/NPCs casting into the sun cascades.
        ImGui::Checkbox("Mesh shadow casters", &meshShadowCastersUi);
        ImGui::Checkbox("Contact shadows", &contactShadowsUi); // brick 33a
        ImGui::SameLine();
        ImGui::Checkbox("Terrain light map", &terrainLightUi); // brick 33b/c
        ImGui::Checkbox("Key light shadow", &keyShadowUi); // B2b (interiors)
    }
    if (ImGui::CollapsingHeader("Sun FX")) {
        ImGui::SliderFloat("God rays intensity", &atmos.godRayIntensity,
                           0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Volumetric shafts", &atmos.volumetric, 0.0f,
                           3.0f, "%.2f");
        ImGui::Checkbox("Light shafts (dust, brick 34)", &shaftsUi);
    }
    if (ImGui::CollapsingHeader("Fog & clouds")) {
        ImGui::SliderFloat("Fog density", &atmos.fogDensity, 0.0f, 0.004f,
                           "%.4f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Fog height falloff", &atmos.fogHeightFalloff,
                           0.001f, 0.08f, "%.3f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Fog low-altitude boost", &atmos.fogLowBoost,
                           0.0f, 5.0f, "%.1f");
        ImGui::SliderFloat("Fog start (m)", &atmos.fogStart, 0.0f, 500.0f,
                           "%.0f");
        ImGui::SliderFloat("Cloud coverage", &atmos.cloudCoverage, 0.0f,
                           1.0f, "%.2f");
        ImGui::SliderFloat("Cloud shadow strength", &atmos.cloudShadow,
                           0.0f, 1.0f, "%.2f");
    }
    if (ImGui::CollapsingHeader("Water")) {
        ImGui::Checkbox("Reflections", &reflectionsUi);
        // GPU-PERF P3: skip the mirror render when no resident water is in
        // view (A/B — the horizon-sea edge case), and trade its resolution.
        ImGui::SameLine();
        ImGui::Checkbox("auto-skip", &reflectionAutoSkipUi);
        ImGui::SliderFloat("Reflection scale", &reflectionScaleUi, 0.25f,
                           0.5f, "%.2f");
    }
    if (ImGui::CollapsingHeader("Post-processing")) {
        ImGui::Checkbox("Filmic tonemap (A/B)", &tonemapUi);
        ImGui::SliderFloat("Exposure", &exposureUi, 0.25f, 3.0f, "%.2f");
        // B4 A/B (brick 29): eye adaptation; Exposure becomes the bias.
        ImGui::Checkbox("Auto exposure (brick 29)", &autoExposureUi);
        if (autoExposureUi) {
            ImGui::SliderFloat("Auto-expo min", &autoExposureMinUi, 0.1f,
                               1.0f, "%.2f");
            ImGui::SliderFloat("Auto-expo max", &autoExposureMaxUi, 1.0f,
                               6.0f, "%.2f");
        }
        ImGui::SliderFloat("Bloom intensity", &atmos.bloomIntensity, 0.0f,
                           1.5f, "%.2f");
        // B3 A/B (brick 28): the analytical grade, off by default.
        ImGui::Checkbox("Grading (brick 28)", &gradingUi);
        if (gradingUi) {
            ImGui::SliderFloat("Vibrance", &gradeVibranceUi, 0.0f, 1.0f,
                               "%.2f");
            ImGui::SliderFloat("Split tone", &gradeSplitToneUi, 0.0f, 1.0f,
                               "%.2f");
            ImGui::SliderFloat("Contrast", &gradeContrastUi, 0.8f, 1.4f,
                               "%.2f");
        }
        ImGui::Combo("Debug buffer", &debugBufferUi,
                     "Off\0Bloom\0God rays\0Volumetric\0");
    }
}

} // namespace game
