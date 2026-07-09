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
    ssaoUi = tuning.ssaoStrength;
    gradeVibranceUi = tuning.gradeVibrance;   // B3 (toggle stays off)
    gradeSplitToneUi = tuning.gradeSplitTone;
    gradeContrastUi = tuning.gradeContrast;
    autoExposureMinUi = tuning.autoExposureMin; // B4 (toggle stays off)
    autoExposureMaxUi = tuning.autoExposureMax;
}

void LandscapeRenderer::create(rhi::Device& device, core::JobSystem& jobs) {
    frameUbo = device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                     .size = sizeof(render::FrameUniforms),
                                     .dynamic = true },
                                   nullptr);
    // B5: local lights ride binding 5 of the SAME group — shaders that
    // don't declare the block simply ignore it.
    lightsUbo = device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          // B1: + the appended direction/angle array (the UBO lesson:
          // new members go at the END, both CPU and GLSL sides).
          .size = (1 + 3 * kMaxLights) * sizeof(Vec4),
          .dynamic = true },
        nullptr);
    frameBindGroup = device.createBindGroup(
        { .entries = { { .binding = 0, .buffer = frameUbo },
                       { .binding = 5, .buffer = lightsUbo } } });

    shaders = std::make_unique<render::ShaderLibrary>(device);
    terrain.create(device, *shaders, jobs);
    occlusion.create(jobs);
    terrainLightMap.create(device, jobs); // 33b/c
    grass.create(device, *shaders, jobs);
    vegetation.create(device, *shaders, jobs,
                      terrain.params.seed);

    // (The mesh/texture residency caches stay SCENE-owned — the editor and
    // streaming share them; the view hands them in per frame.)
    const u32 white = 0xFFFFFFFF;
    whiteTexture = device.createTexture(
        { .width = 1, .height = 1, .format = rhi::TextureFormat::SRGBA8 },
        &white);
    meshSampler = device.createSampler({});
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
    // Brick 30: horizon cumulonimbus — 8 towers, static vertex buffer
    // (the vertex shader anchors the ring to the camera).
    shaders->load("cumulonimbus", { { "FrameUbo", 0 } });
    {
        f32 towers[8 * 6 * 4];
        u32 cursor = 0;
        const auto push = [&](f32 azimuth, f32 u, f32 v, f32 seed) {
            towers[cursor++] = azimuth;
            towers[cursor++] = u;
            towers[cursor++] = v;
            towers[cursor++] = seed;
        };
        for (u32 i = 0; i < 8; ++i) {
            const f32 azimuth =
                static_cast<f32>(i) * glm::radians(45.0f) + 0.37f;
            const f32 seed = static_cast<f32>(i) * 0.618f -
                             std::floor(static_cast<f32>(i) * 0.618f);
            push(azimuth, -1.0f, 0.0f, seed);
            push(azimuth, 1.0f, 0.0f, seed);
            push(azimuth, 1.0f, 1.0f, seed);
            push(azimuth, -1.0f, 0.0f, seed);
            push(azimuth, 1.0f, 1.0f, seed);
            push(azimuth, -1.0f, 1.0f, seed);
        }
        stormVertices = device.createBuffer(
            { .usage = rhi::BufferUsage::Vertex, .size = sizeof(towers) },
            towers);
    }

    // Brick 31: rain — procedural streaks (no buffers) + the top-down
    // occlusion depth so roofs keep the drops out.
    shaders->load("rain", { { "FrameUbo", 0 } },
                  { { "uRainOcclusion", 9 } });
    rainOcclusionTex = device.createTexture(
        { .width = 512,
          .height = 512,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr);
    rainSampler = device.createSampler({});
    rainOcclusionFb = device.createFramebuffer(
        { .depthAttachment = { .texture = rainOcclusionTex } });
    rainOcclusionUbo =
        device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                              .size = sizeof(Mat4),
                              .dynamic = true },
                            nullptr);
    rainCasterGroup = device.createBindGroup(
        { .entries = { { .binding = 1, .buffer = rainOcclusionUbo } } });
    rainReceiverGroup = device.createBindGroup(
        { .entries = { { .binding = 9,
                         .texture = rainOcclusionTex,
                         .sampler = rainSampler } } });

    // B2b: the interior key-light shadow target (1024², perspective).
    keyShadowTex = device.createTexture(
        { .width = 1024,
          .height = 1024,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr);
    keyShadowSampler = device.createSampler(
        { .minFilter = rhi::FilterMode::Linear,
          .magFilter = rhi::FilterMode::Linear,
          .compare = rhi::CompareFunc::LessEqual });
    keyShadowFb = device.createFramebuffer(
        { .depthAttachment = { .texture = keyShadowTex } });
    keyShadowUbo =
        device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                              .size = sizeof(Mat4),
                              .dynamic = true },
                            nullptr);
    keyShadowCasterGroup = device.createBindGroup(
        { .entries = { { .binding = 1, .buffer = keyShadowUbo } } });
    keyShadowReceiverGroup = device.createBindGroup(
        { .entries = { { .binding = 6,
                         .texture = keyShadowTex,
                         .sampler = keyShadowSampler } } });

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
        depthSampler = device.createSampler(
            { .minFilter = rhi::FilterMode::Nearest,
              .magFilter = rhi::FilterMode::Nearest });
        reflectionUbo =
            device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                  .size = sizeof(render::FrameUniforms),
                                  .dynamic = true },
                                nullptr);
        reflectionBindGroup = device.createBindGroup(
            { .entries = { { .binding = 0, .buffer = reflectionUbo } } });
    }

    if (device.caps().offscreenTargets) {
        blitSampler = device.createSampler({}); // linear, clamp — identity
        shaders->load(kTonemapShader, { { "FrameUbo", 0 } },
                      { { "uSceneColor", 0 },
                        { "uBloom", 1 },
                        { "uGodRays", 2 },
                        { "uVolumetric", 3 },
                        { "uSsao", 4 },
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
    destroyOffscreenTarget(device);
    device.destroyPipeline(blitPipeline);
    device.destroySampler(blitSampler);
    // B1 mesh path: per-entry draw state (the residency caches are
    // scene-owned; their dtors free what they own).
    for (MeshDraw& draw : meshDraws) {
        if (draw.group.id != 0) {
            device.destroyBindGroup(draw.group);
        }
        if (draw.casterGroup.id != 0) {
            device.destroyBindGroup(draw.casterGroup);
        }
        if (draw.ubo.id != 0) {
            device.destroyBuffer(draw.ubo);
        }
    }
    meshDraws.clear();
    device.destroyPipeline(meshPipeline);
    device.destroyPipeline(meshCasterPipeline);   // B2a
    device.destroyPipeline(skinnedCasterPipeline);
    // Brick 34: shafts (GPU state per shaft, then the pipeline).
    for (LightShaft& shaft : lightShafts) {
        if (shaft.vertices.id != 0) {
            device.destroyBuffer(shaft.vertices);
        }
        if (shaft.ubo.id != 0) {
            device.destroyBindGroup(shaft.group);
            device.destroyBuffer(shaft.ubo);
        }
    }
    lightShafts.clear();
    device.destroyPipeline(shaftPipeline);
    shaftPipeline = {};
    // Brick 32: water quads.
    for (WaterQuad& quad : waterQuads) {
        if (quad.vertices.id != 0) {
            device.destroyBuffer(quad.vertices);
            device.destroyBindGroup(quad.group);
            device.destroyBuffer(quad.ubo);
        }
    }
    waterQuads.clear();
    device.destroyPipeline(waterVolumePipeline);
    waterVolumePipeline = {};
    // Brick 30: cumulonimbus.
    device.destroyBuffer(stormVertices);
    stormVertices = {};
    device.destroyPipeline(stormPipeline);
    stormPipeline = {};
    // Brick 31: rain.
    device.destroyPipeline(rainPipeline);
    rainPipeline = {};
    device.destroyBindGroup(rainReceiverGroup);
    device.destroyBindGroup(rainCasterGroup);
    device.destroyBuffer(rainOcclusionUbo);
    device.destroyFramebuffer(rainOcclusionFb);
    device.destroySampler(rainSampler);
    device.destroyTexture(rainOcclusionTex);
    rainReceiverGroup = {};
    rainCasterGroup = {};
    rainOcclusionUbo = {};
    rainOcclusionFb = {};
    rainSampler = {};
    rainOcclusionTex = {};
    // B2b: key-light shadow.
    device.destroyBindGroup(keyShadowReceiverGroup);
    device.destroyBindGroup(keyShadowCasterGroup);
    device.destroyBuffer(keyShadowUbo);
    device.destroyFramebuffer(keyShadowFb);
    device.destroySampler(keyShadowSampler);
    device.destroyTexture(keyShadowTex);
    keyShadowReceiverGroup = {};
    keyShadowCasterGroup = {};
    keyShadowUbo = {};
    keyShadowFb = {};
    keyShadowSampler = {};
    keyShadowTex = {};
    // B6 NPCs: the per-entity draw state (U4-2b).
    for (SkinnedDraw& draw : skinnedDraws) {
        if (draw.casterGroup.id != 0) {
            device.destroyBindGroup(draw.casterGroup);
        }
        if (draw.group.id != 0) {
            device.destroyBindGroup(draw.group);
            device.destroyBuffer(draw.modelUbo);
            device.destroyBuffer(draw.paletteSsbo);
        }
    }
    skinnedDraws.clear();
    device.destroyPipeline(skinnedPipeline);
    skinnedPipeline = {};
    skinnedShaderGeneration = 0;
    device.destroySampler(meshSampler);
    device.destroyTexture(whiteTexture);
    gpuOcclusion.destroy(device);
    terrainLightMap.destroy(device); // 33b/c
    postFx.destroy(device);
    water.destroy(device);
    device.destroyBindGroup(reflectionBindGroup);
    device.destroyBuffer(reflectionUbo);
    device.destroySampler(depthSampler);
    shadows.destroy(device);
    sky.destroy(device);
    vegetation.destroy(device);
    grass.destroy(device);
    terrain.destroy(device);
    shaders.reset(); // destroys the library's shader programs
    device.destroyBindGroup(frameBindGroup);
    device.destroyBuffer(lightsUbo);
    device.destroyBuffer(frameUbo);
    frameBindGroup = {};
    lightsUbo = {};
    frameUbo = {};
    sculptDirtyChunks.clear();
    sculptScatterChunks.clear();
}

void LandscapeRenderer::ensureOffscreenTarget(rhi::Device& device, u32 width,
                                           u32 height) {
    if (offscreenFb.id != 0 && offscreenWidth == width &&
        offscreenHeight == height) {
        return;
    }
    destroyOffscreenTarget(device);
    // HDR scene target: the sky/sun palette is linear HDR (sun > 1); the
    // tonemap pass compresses to display range.
    offscreenColor = device.createTexture(
        { .width = width,
          .height = height,
          .format = device.caps().hdrFormats ? rhi::TextureFormat::RGBA16F
                                             : rhi::TextureFormat::RGBA8,
          .filter = rhi::FilterMode::Linear,
          .usage = rhi::TextureUsage_Sampled |
                   rhi::TextureUsage_RenderAttachment },
        nullptr);
    offscreenDepth = device.createTexture(
        { .width = width,
          .height = height,
          .format = rhi::TextureFormat::Depth32F,
          .usage = rhi::TextureUsage_RenderAttachment },
        nullptr);
    offscreenFb = device.createFramebuffer(
        { .colorAttachments = { { .texture = offscreenColor } },
          .depthAttachment = { .texture = offscreenDepth } });
    if (device.caps().copyTexture) {
        sceneColorCopy = device.createTexture(
            { .width = width,
              .height = height,
              .format = device.caps().hdrFormats ? rhi::TextureFormat::RGBA16F
                                                 : rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled },
            nullptr);
        sceneDepthCopy = device.createTexture(
            { .width = width,
              .height = height,
              .format = rhi::TextureFormat::Depth32F,
              .usage = rhi::TextureUsage_Sampled },
            nullptr);
        const u32 reflectionWidth = glm::max(width / 2, 1u);
        const u32 reflectionHeight = glm::max(height / 2, 1u);
        reflectionColor = device.createTexture(
            { .width = reflectionWidth,
              .height = reflectionHeight,
              .format = device.caps().hdrFormats ? rhi::TextureFormat::RGBA16F
                                                 : rhi::TextureFormat::RGBA8,
              .filter = rhi::FilterMode::Linear,
              .usage = rhi::TextureUsage_Sampled |
                       rhi::TextureUsage_RenderAttachment },
            nullptr);
        reflectionDepth = device.createTexture(
            { .width = reflectionWidth,
              .height = reflectionHeight,
              .format = rhi::TextureFormat::Depth32F,
              .usage = rhi::TextureUsage_RenderAttachment },
            nullptr);
        reflectionFb = device.createFramebuffer(
            { .colorAttachments = { { .texture = reflectionColor } },
              .depthAttachment = { .texture = reflectionDepth } });
        waterSceneBindGroup = device.createBindGroup(
            { .entries = { { .binding = 0,
                             .texture = sceneColorCopy,
                             .sampler = blitSampler },
                           { .binding = 1,
                             .texture = sceneDepthCopy,
                             .sampler = depthSampler },
                           { .binding = 2,
                             .texture = reflectionColor,
                             .sampler = blitSampler } } });
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
        blitBindGroups[side] = device.createBindGroup(
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
                              .texture = postFx.ssaoTexture(),
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
                              .sampler = blitSampler } } });
    }
    offscreenWidth = width;
    offscreenHeight = height;
}

void LandscapeRenderer::destroyOffscreenTarget(rhi::Device& device) {
    if (offscreenFb.id == 0) {
        return;
    }
    device.destroyBindGroup(waterSceneBindGroup);
    device.destroyFramebuffer(reflectionFb);
    device.destroyTexture(reflectionDepth);
    device.destroyTexture(reflectionColor);
    device.destroyTexture(sceneDepthCopy);
    device.destroyTexture(sceneColorCopy);
    waterSceneBindGroup = {};
    reflectionFb = {};
    reflectionDepth = {};
    reflectionColor = {};
    sceneDepthCopy = {};
    sceneColorCopy = {};
    device.destroyBindGroup(blitBindGroups[0]);
    device.destroyBindGroup(blitBindGroups[1]);
    device.destroyFramebuffer(offscreenFb);
    device.destroyTexture(offscreenDepth);
    device.destroyTexture(offscreenColor);
    blitBindGroups = {};
    offscreenFb = {};
    offscreenDepth = {};
    offscreenColor = {};
    offscreenWidth = 0;
    offscreenHeight = 0;
}

void LandscapeRenderer::rebuildBlitPipeline(rhi::Device& device) {
    if (blitPipeline.id != 0) {
        device.destroyPipeline(blitPipeline);
    }
    blitPipeline =
        device.createPipeline({ .shader = shaders->get(kTonemapShader) });
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
    struct ModelUniforms { // std140 ModelUbo: model + tint + info
        Mat4 model { 1.0f };
        Vec4 tint { 1.0f };
        Vec4 info { 0.0f }; // x = emissive
    };
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
        if (draw.ubo.id == 0) {
            draw.ubo = frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  .size = sizeof(ModelUniforms),
                  .dynamic = true },
                nullptr);
        }
        frame.device.updateBuffer(draw.ubo, &uniforms, sizeof(uniforms), 0);
        if (draw.group.id == 0 || draw.boundTexture.id != albedo.id ||
            draw.material != instance.material) {
            if (draw.group.id != 0) {
                frame.device.destroyBindGroup(draw.group);
            }
            draw.group = frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = draw.ubo },
                               { .binding = 0,
                                 .texture = albedo,
                                 .sampler = meshSampler } } });
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
    struct ModelUniforms {
        Mat4 model { 1.0f };
        Vec4 tint { 1.0f };
        Vec4 info { 0.0f };
    };
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
        if (slot->paletteSsbo.id == 0) {
            slot->paletteSsbo = frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Storage,
                  .size = instance.palette.size() * sizeof(Mat4),
                  .dynamic = true },
                instance.palette.data());
            slot->modelUbo = frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  // std140 ModelUbo: mat4 model + vec4 tint + vec4 info.
                  .size = sizeof(Mat4) + 2 * sizeof(Vec4),
                  .dynamic = true },
                nullptr);
            slot->group = frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = slot->modelUbo },
                               { .binding = 0,
                                 .texture = whiteTexture,
                                 .sampler = meshSampler },
                               { .binding = 2,
                                 .buffer = slot->paletteSsbo,
                                 .storage = true } } });
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
            if (it->casterGroup.id != 0) {
                frame.device.destroyBindGroup(it->casterGroup);
            }
            if (it->group.id != 0) {
                frame.device.destroyBindGroup(it->group);
                frame.device.destroyBuffer(it->modelUbo);
                frame.device.destroyBuffer(it->paletteSsbo);
            }
            it = skinnedDraws.erase(it);
        } else {
            ++it;
        }
    }
}

void LandscapeRenderer::buildSkinnedPipeline(rhi::Device& device) {
    if (skinnedPipeline.id != 0) {
        device.destroyPipeline(skinnedPipeline);
    }
    skinnedPipeline = device.createPipeline(
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
          .cull = rhi::CullMode::Back });
    skinnedShaderGeneration = shaders->generation("skinned");
}

// Bundle the streaming fixups' systems for StreamingController this frame —
// references into the scene plus the focus / fade / mode scalars. Rebuilt each

void LandscapeRenderer::buildShaftPipeline(rhi::Device& device) {
    if (shaftPipeline.id != 0) {
        device.destroyPipeline(shaftPipeline);
    }
    // Additive, depth-tested against the opaques but never writing —
    // the Skyrim FXShaft blend. Both blade faces show (no cull).
    shaftPipeline = device.createPipeline(
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
          .cull = rhi::CullMode::None });
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
        if (slot->vertices.id == 0 ||
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
            if (slot->vertices.id == 0) {
                slot->vertices = frame.device.createBuffer(
                    { .usage = rhi::BufferUsage::Vertex,
                      .size = sizeof(verts),
                      .dynamic = true },
                    verts);
            } else {
                frame.device.updateBuffer(slot->vertices, verts,
                                          sizeof(verts), 0);
            }
            slot->vertexCount = 18;
            slot->cachedDir = dir;
        }
        if (slot->ubo.id == 0) {
            slot->ubo = frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  .size = 2 * sizeof(Vec4),
                  .dynamic = true },
                nullptr);
            slot->group = frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = slot->ubo } } });
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
            if (it->vertices.id != 0) {
                frame.device.destroyBuffer(it->vertices);
            }
            if (it->ubo.id != 0) {
                frame.device.destroyBindGroup(it->group);
                frame.device.destroyBuffer(it->ubo);
            }
            it = lightShafts.erase(it);
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
        waterVolumePipeline.id == 0) {
        if (waterVolumePipeline.id != 0) {
            frame.device.destroyPipeline(waterVolumePipeline);
        }
        waterVolumePipeline = frame.device.createPipeline(
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
              .cull = rhi::CullMode::None });
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
        if (slot->vertices.id == 0) {
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
            slot->vertices = frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex, .size = sizeof(verts) },
                verts);
            slot->ubo = frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  .size = sizeof(Vec4),
                  .dynamic = true },
                nullptr);
            slot->group = frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = slot->ubo } } });
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
            if (it->vertices.id != 0) {
                frame.device.destroyBuffer(it->vertices);
                frame.device.destroyBindGroup(it->group);
                frame.device.destroyBuffer(it->ubo);
            }
            it = waterQuads.erase(it);
        } else {
            ++it;
        }
    }
}

void LandscapeRenderer::buildCasterPipelines(rhi::Device& device) {
    if (meshCasterPipeline.id != 0) {
        device.destroyPipeline(meshCasterPipeline);
    }
    if (skinnedCasterPipeline.id != 0) {
        device.destroyPipeline(skinnedCasterPipeline);
    }
    // Position-only attributes over the FULL vertex strides (same buffers
    // as the lit pass); depth state mirrors terrain/vegetation casters.
    meshCasterPipeline = device.createPipeline(
        { .shader = shaders->get("shadow_mesh"),
          .vertexBuffers =
              { { .stride = sizeof(render::MeshVertex),
                  .attributes =
                      { { .location = 0,
                          .format = rhi::VertexFormat::F32x3,
                          .offset =
                              offsetof(render::MeshVertex, position) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back });
    skinnedCasterPipeline = device.createPipeline(
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
          .cull = rhi::CullMode::Back });
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
            if (draw.ubo.id == 0) {
                // std140 ModelUbo: mat4 + tint + info (drawSceneMeshes
                // owns the tail; only the matrix matters here).
                draw.ubo = frame.device.createBuffer(
                    { .usage = rhi::BufferUsage::Uniform,
                      .size = sizeof(Mat4) + 2 * sizeof(Vec4),
                      .dynamic = true },
                    nullptr);
            }
            if (firstCascade) {
                frame.device.updateBuffer(draw.ubo, &instance.transform,
                                          sizeof(Mat4), 0);
            }
            if (draw.casterGroup.id == 0) {
                draw.casterGroup = frame.device.createBindGroup(
                    { .entries = { { .binding = 4, .buffer = draw.ubo } } });
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
            if (!slot || slot->modelUbo.id == 0) {
                continue; // built by drawNpcs later this frame
            }
            if (slot->casterGroup.id == 0) {
                slot->casterGroup = frame.device.createBindGroup(
                    { .entries = { { .binding = 4,
                                     .buffer = slot->modelUbo },
                                   { .binding = 2,
                                     .buffer = slot->paletteSsbo,
                                     .storage = true } } });
            }
            frame.cmd.setBindGroup(2, slot->casterGroup);
            frame.cmd.setVertexBuffer(0, instance.vertices);
            frame.cmd.setIndexBuffer(instance.indices, rhi::IndexFormat::U32);
            frame.cmd.drawIndexed(instance.indexCount);
        }
    }
}

void LandscapeRenderer::buildMeshPipeline(rhi::Device& device) {
    if (meshPipeline.id != 0) {
        device.destroyPipeline(meshPipeline);
    }
    meshPipeline = device.createPipeline(
        { .shader = shaders->get("mesh"),
          .vertexBuffers =
              { { .stride = sizeof(render::MeshVertex),
                  .attributes =
                      { { .location = 0,
                          .format = rhi::VertexFormat::F32x3,
                          .offset = offsetof(render::MeshVertex, position) },
                        { .location = 1,
                          .format = rhi::VertexFormat::F32x3,
                          .offset = offsetof(render::MeshVertex, normal) },
                        { .location = 2,
                          .format = rhi::VertexFormat::F32x2,
                          .offset = offsetof(render::MeshVertex, uv) },
                        { .location = 3,
                          .format = rhi::VertexFormat::F32x3,
                          .offset = offsetof(render::MeshVertex, color) } } } },
          .depth = { .testEnable = true,
                     .writeEnable = true,
                     .compare = rhi::CompareFunc::Less },
          .cull = rhi::CullMode::Back });
    meshShaderGeneration = shaders->generation("mesh");
}

void LandscapeRenderer::render(engine::FrameContext& frame,
                               const RenderSnapshot& snapshot,
                               const RenderView& view) {
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
    terrain.setWireframe(wireframeUi, frame.device, *shaders);
    if (regenerateRequested) {
        regenerateRequested = false;
        terrain.regenerate(frame.device);
        grass.regenerate(frame.device);
        vegetation.regenerate(frame.device, terrain.params.seed);
        occlusion.invalidate();
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
    if (glm::dot(shadowSunDirection, skyState.sunDirection) <
        std::cos(glm::radians(0.4f))) {
        shadowSunDirection = skyState.sunDirection;
    }
    render::ShadowMapper::Cascades cascades {};
    if (shadowStrength > 0.0f) {
        cascades = shadows.computeCascades(camera, frame.aspect,
                                           shadowSunDirection);
        shadows.updateCascadeUbos(frame.device, cascades);
    }

    // Planar reflection is meaningful only from above the surface.
    const bool reflectionsActive =
        reflectionsUi && reflectionFb.id != 0 && !view.interiorMode &&
        camera.position.y > terrain.params.seaLevel;

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
        .ssao = ssaoUi,
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
        struct LightsUniforms {
            Vec4 count { 0.0f };
            Vec4 positionRadius[kMaxLights] {};
            Vec4 colorIntensity[kMaxLights] {};
            // B1 APPEND (mirrors locallights.glsl): xyz = spot direction,
            // w = cos(half angle); w = -2 marks a point light.
            Vec4 directionAngle[kMaxLights] {};
        } lights;
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
        for (u32 i = 0; i < render::ShadowMapper::kCascadeCount; ++i) {
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

    // Exterior: the sky covers every background pixel — no color clear.
    // Interior: clear to a near-black room tone instead.
    {
        core::FrameProbe::Scope probe { *view.probe, "mainPass" };
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
        if (keyShadowReceiverGroup.id != 0) {
            frame.cmd.setBindGroup(5, keyShadowReceiverGroup); // B2b
        }
        // Occlusion applies to the main view only: both sets were built for
        // the real camera, not the mirrored one (the grass ring is too
        // close to ever be ridge-occluded — frustum only). CPU ∪ GPU Hi-Z.
        gpuOccluded.clear();
        gpuOcclusion.collectResults(frame.device, gpuOccluded);
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
            terrain.draw(frame.cmd, frameBindGroup,
                         shadows.receiverBindGroup(), &viewFrustum,
                         occludedSet);
            vegetation.draw(frame.cmd, frameBindGroup,
                            shadows.receiverBindGroup(),
                            render::VegetationSystem::kVariantCount,
                            camera.position,
                            /*forceLowDetail=*/false, &viewFrustum,
                            occludedSet);
            grass.draw(frame.cmd, frameBindGroup,
                       shadows.receiverBindGroup(), camera.position,
                       &viewFrustum);
        }
        drawSceneMeshes(frame, snapshot, view); // B1: the RenderSnapshot.meshes consumer
        drawSkinned(frame, snapshot);        // B6: the Forms-driven skinned NPCs
        if (!view.interiorMode) {
            sky.draw(frame.cmd, frameBindGroup); // background only
        }
        // Brick 30: horizon cumulonimbus, right after the sky dome (they
        // occlude sky, terrain occludes them via the depth test).
        if (!view.interiorMode && view.atmos.stormFront > 0.003f) {
            if (shaders->generation("cumulonimbus") !=
                    stormShaderGeneration ||
                stormPipeline.id == 0) {
                if (stormPipeline.id != 0) {
                    frame.device.destroyPipeline(stormPipeline);
                }
                stormPipeline = frame.device.createPipeline(
                    { .shader = shaders->get("cumulonimbus"),
                      .vertexBuffers =
                          { { .stride = 4 * sizeof(f32),
                              .attributes =
                                  { { .location = 0,
                                      .format = rhi::VertexFormat::F32x4,
                                      .offset = 0 } } } },
                      .blend = rhi::BlendMode::Alpha,
                      .depth = { .testEnable = true,
                                 .writeEnable = false,
                                 .compare = rhi::CompareFunc::Less },
                      .cull = rhi::CullMode::None });
                stormShaderGeneration =
                    shaders->generation("cumulonimbus");
            }
            frame.cmd.setPipeline(stormPipeline);
            frame.cmd.setBindGroup(0, frameBindGroup);
            frame.cmd.setVertexBuffer(0, stormVertices);
            frame.cmd.draw(8 * 6);
        }
        // Brick 32: placed water surfaces (alpha), then brick 34:
        // additive dust shafts — both after every opaque.
        drawWaterVolumes(frame, snapshot);
        drawLightShafts(frame, snapshot, view, skyState.sunColor);
        // Brick 31: rain streaks (procedural, camera cylinder).
        if (frameData.stormInfo.y > 0.003f) {
            if (shaders->generation("rain") != rainShaderGeneration ||
                rainPipeline.id == 0) {
                if (rainPipeline.id != 0) {
                    frame.device.destroyPipeline(rainPipeline);
                }
                rainPipeline = frame.device.createPipeline(
                    { .shader = shaders->get("rain"),
                      .blend = rhi::BlendMode::Alpha,
                      .depth = { .testEnable = true,
                                 .writeEnable = false,
                                 .compare = rhi::CompareFunc::Less },
                      .cull = rhi::CullMode::None });
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
        waterSceneBindGroup.id != 0) {
        core::FrameProbe::Scope probe { *view.probe, "copyHizWater" };
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
                      shadows.receiverBindGroup());
        // 33a: contact shadows (the texture is the toggle — white = off).
        if (contactShadowsUi && !view.interiorMode) {
            postFx.renderContactShadows(frame.cmd, frameBindGroup);
        } else {
            postFx.clearContactShadows(frame.cmd);
        }
        // B4 (brick 29): measure + adapt, before the tonemap taps it.
        if (autoExposureUi) {
            postFx.renderAutoExposure(frame.device, frame.cmd,
                                      frameBindGroup);
        }
    }

    if (useOffscreen) {
        core::FrameProbe::Scope probe { *view.probe, "composite" };
        // Tonemap composite: HDR scene -> filmic curve -> gamma -> backbuffer.
        frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::DontCare });
        frame.cmd.setPipeline(blitPipeline);
        frame.cmd.setBindGroup(0, frameBindGroup); // FrameUbo (uPostInfo)
        // B4: the side the adaptation pass just wrote.
        frame.cmd.setBindGroup(1, blitBindGroups[postFx.exposureSide()]);
        frame.cmd.draw(3);
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
void LandscapeRenderer::drawTerrainPanel() {
        ImGui::Text("Resident: %u | drawn: %u | pending: %u | uploads: %u",
                    terrain.residentCount(), terrain.drawnLastFrame(),
                    terrain.pendingCount(), terrain.uploadsLastFrame());
        ImGui::Text("Prop chunks drawn: %u | occluded CPU: %u | GPU: %u",
                    vegetation.drawnLastFrame(), occlusion.occludedCount(),
                    gpuOcclusion.lastOccludedCount());
        ImGui::Checkbox("Occlusion culling (A/B)", &occlusionUi);
        ImGui::SameLine();
        ImGui::Checkbox("GPU Hi-Z", &gpuOcclusionUi);
        ImGui::Text("Grass blades: %u | props: %u", grass.instanceTotal(),
                    vegetation.propTotal());
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
        ImGui::Checkbox("Wireframe (LOD debug)", &wireframeUi);
}

void LandscapeRenderer::drawRenderPanel(AtmosphereParams& atmos) {
    ImGui::Checkbox("Stylized lighting (BotW A/B)", &stylizedUi);
    ImGui::Checkbox("Filmic tonemap (A/B)", &tonemapUi);
    ImGui::SliderFloat("Bloom intensity", &atmos.bloomIntensity, 0.0f, 1.5f,
                       "%.2f");
    ImGui::SliderFloat("God rays intensity", &atmos.godRayIntensity, 0.0f, 2.0f,
                       "%.2f");
    ImGui::SliderFloat("Volumetric shafts", &atmos.volumetric, 0.0f, 3.0f,
                       "%.2f");
    ImGui::SliderFloat("SSAO strength", &ssaoUi, 0.0f, 1.0f, "%.2f");
    ImGui::Combo("Debug buffer", &debugBufferUi,
                 "Off\0Bloom\0God rays\0Volumetric\0SSAO\0");
    ImGui::Checkbox("Shadows", &shadowsUi);
    ImGui::SameLine();
    ImGui::Checkbox("Cascade debug tint", &cascadeDebugUi);
    // B2a A/B: houses/crates/NPCs casting into the sun cascades.
    ImGui::Checkbox("Mesh shadow casters", &meshShadowCastersUi);
    ImGui::SameLine();
    ImGui::Checkbox("Light shafts", &shaftsUi); // brick 34
    ImGui::Checkbox("Contact shadows", &contactShadowsUi); // brick 33a
    ImGui::SameLine();
    ImGui::Checkbox("Terrain light map", &terrainLightUi); // brick 33b/c
    ImGui::Checkbox("Key light shadow", &keyShadowUi); // B2b (interiors)
    // B3 A/B (brick 28): the analytical grade, off by default.
    ImGui::Checkbox("Grading (brick 28)", &gradingUi);
    if (gradingUi) {
        ImGui::SliderFloat("Vibrance", &gradeVibranceUi, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Split tone", &gradeSplitToneUi, 0.0f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Contrast", &gradeContrastUi, 0.8f, 1.4f, "%.2f");
    }
    ImGui::Checkbox("Water reflections", &reflectionsUi);
    ImGui::SliderFloat("Exposure", &exposureUi, 0.25f, 3.0f, "%.2f");
    // B4 A/B (brick 29): eye adaptation; Exposure above becomes the bias.
    ImGui::Checkbox("Auto exposure (brick 29)", &autoExposureUi);
    if (autoExposureUi) {
        ImGui::SliderFloat("Auto-expo min", &autoExposureMinUi, 0.1f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Auto-expo max", &autoExposureMaxUi, 1.0f, 6.0f,
                           "%.2f");
    }
    ImGui::SliderFloat("Fog density", &atmos.fogDensity, 0.0f, 0.004f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Fog height falloff", &atmos.fogHeightFalloff, 0.001f,
                       0.08f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Fog low-altitude boost", &atmos.fogLowBoost, 0.0f, 5.0f,
                       "%.1f");
    ImGui::SliderFloat("Fog start (m)", &atmos.fogStart, 0.0f, 500.0f, "%.0f");
    ImGui::SliderFloat("Cloud coverage", &atmos.cloudCoverage, 0.0f, 1.0f,
                       "%.2f");
    ImGui::SliderFloat("Cloud shadow strength", &atmos.cloudShadow, 0.0f, 1.0f,
                       "%.2f");
}

} // namespace game
