#include "engine/render/WorldRenderer.hpp"

#include "engine/core/Assert.hpp"

#include <cmath>

#include <algorithm>
#include <cstdio>
#include <iterator>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/FrameContext.hpp"
#include "engine/assets/MeshData.hpp" // render::MeshVertex / SkinnedVertex
#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/MeshVertexLayout.hpp"
#include "engine/terrain/WaterQuery.hpp"
#include "engine/render/Projection.hpp"
#include "engine/render/landscape/FrameUniforms.hpp"
#include "engine/render/landscape/TerrainNoise.hpp"
#include "engine/render/MeshCache.hpp"
#include "engine/render/TextureCache.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/ui/UiSystem.hpp"
#include "engine/render/FrameComposer.hpp"
#include "engine/render/PipelinePresets.hpp"

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

// World-space AABB of a mesh instance: the local bounds through the
// model matrix, corner-expanded. Shared by the caster/main-pass cull
// and the GI box inject.
void instanceWorldBounds(const Mat4& transform,
                         const MeshCache::CpuMesh& cpu, Vec3& lo,
                         Vec3& hi) {
    lo = Vec3 { 1e9f };
    hi = Vec3 { -1e9f };
    for (u32 corner = 0; corner < 8; ++corner) {
        const Vec3 local {
            (corner & 1) ? cpu.boundsMax.x : cpu.boundsMin.x,
            (corner & 2) ? cpu.boundsMax.y : cpu.boundsMin.y,
            (corner & 4) ? cpu.boundsMax.z : cpu.boundsMin.z
        };
        const Vec3 world = Vec3(transform * Vec4 { local, 1.0f });
        lo = glm::min(lo, world);
        hi = glm::max(hi, world);
    }
}

// Frustum test for one snapshot mesh. Conservative: a mesh whose CPU
// bounds are not resident yet (placeholder box while decoding) always
// passes — resolve() must keep being called so it loads at all.
bool instanceVisible(const Frustum* frustum, const Mat4& transform,
                     const MeshCache::CpuMesh* cpu) {
    if (!frustum || !cpu) {
        return true;
    }
    Vec3 lo;
    Vec3 hi;
    instanceWorldBounds(transform, *cpu, lo, hi);
    return frustum->intersectsAabb(lo, hi);
}

// Conservative bounding sphere of a skinned NPC (translate x rotation
// transform, no scale): torso anchor + reach for limbs and weapon.
constexpr f32 kSkinnedCasterRadius = 3.0f;

// The two far rings (docs/RENDERING.md §3.6, §6.0c), indexed by
// WorldRenderer::kInnerRing / kHorizonRing. The inner ring is
// FarTerrain's default (18 km, tree impostors); the horizon ring is the
// same machinery at world scale — no impostors (the canopy raise is the
// trees out there), sampled off the L3 pyramid level, ridden 16 m under
// the inner ring so the two never z-fight. The sea quad spans the
// farthest enabled ring: its half span derives from these.
constexpr FarTerrain::Config kFarRingConfigs[] = {
    {},
    { .span = 57600.0f,
      .gridN = 256,
      .treeFar = 0.0f,
      .extraSink = 16.0f,
      .minTexel = 256.0f,
      .probeName = "farHorizon" },
};
static_assert(std::size(kFarRingConfigs) == 2);

// What both far rings bake from, gathered once per frame: the real-tree
// silhouette is a measurement over the whole scatter (not free), the
// layer albedos the painted ground means, the pyramid the half-cell
// grid samples.
struct FarRingInputs {
    VegetationSystem::TreeSilhouette trees {};
    array<Vec3, 5> layerAlbedos {};
    sptr<const HeightField::Snapshot> field;
};

} // namespace

WorldRenderer::Mounted WorldRenderer::resolveMounted(
    const RendererConfig& cfg, const rhi::DeviceCaps& caps) {
    Mounted m;
    m.terrain = cfg.terrain;
    m.mist = cfg.terrain && cfg.postFx;
    m.occlusion = cfg.occlusion;
    m.gi = cfg.gi;
    m.grass = cfg.grass;
    m.vegetation = cfg.vegetation;
    m.sky = cfg.sky;
    m.shadows = caps.offscreenTargets && caps.textureArrays;
    m.fx = caps.copyTexture;
    m.water = cfg.water && caps.copyTexture;
    m.postFx = cfg.postFx && caps.offscreenTargets && caps.hdrFormats &&
               caps.copyTexture;
    m.froxels = cfg.froxels;
    m.gpuOcclusion = cfg.occlusion && caps.computeShaders &&
                     caps.copyTexture && caps.offscreenTargets;
    m.clusters = caps.computeShaders;
    m.offscreen = caps.offscreenTargets;
    m.sceneCopy = caps.copyTexture;
    return m;
}

// The mounted subsystems in creation order; create() and
// refresh/regenerate walk it forwards, destroy() backwards. The order
// carries the dependencies:
//  - the cluster SSBO is created BEFORE the frame bind group (it rides
//    binding 4 of that group) by create() itself; the table entry only
//    builds its pipeline, last, once every shader is loaded;
//  - `terrain` before `vegetation` (the scatter seeds from
//    terrain.params.seed) and before the far rings and the mist map
//    (they bake the same TerrainParams and the shared height pyramid);
//  - `ssdm` before `postFx`: both load fullscreen programs into the
//    shared library, in this order.
// Everything the always-on core owns (frame UBOs, mesh/skinned/rain/key-
// shadow resources, the tonemap) stays hand-written in create().
const WorldRenderer::SystemEntry WorldRenderer::kSystems[] = {
    { "terrain", &Mounted::terrain,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem& j) {
          r.terrain.create(d, *r.shaders, j,
                           { .albedo = r.cfg.terrainAlbedoPath,
                             .normal = r.cfg.terrainNormalPath,
                             .orm = r.cfg.terrainOrmPath,
                             .height = r.cfg.terrainHeightPath });
          r.heightField.create(j);
          r.terrainLightMap.create(d, j);
          r.terrainShadeMap.create(d, j);
          r.fireScorch.create(d);
      },
      [](WorldRenderer& r, rhi::Device& d) {
          r.terrainLightMap.destroy(d);
          r.terrainShadeMap.destroy(d);
          r.fireScorch.destroy(d);
          r.terrain.destroy(d);
      },
      [](WorldRenderer& r, rhi::Device& d) {
          r.terrain.refreshPipeline(d, *r.shaders);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.terrain.regenerate(d); } },
    { "farRings", &Mounted::terrain,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem& j) {
          for (u32 ring = 0; ring < WorldRenderer::kFarRingCount; ++ring) {
              r.farRings[ring].create(d, *r.shaders, j,
                                      kFarRingConfigs[ring]);
          }
      },
      [](WorldRenderer& r, rhi::Device& d) {
          for (FarTerrain& ring : r.farRings) {
              ring.destroy(d);
          }
      },
      [](WorldRenderer& r, rhi::Device& d) {
          for (FarTerrain& ring : r.farRings) {
              ring.refreshPipeline(d, *r.shaders);
          }
      },
      nullptr },
    { "mist", &Mounted::mist,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem& j) {
          r.mistMap.create(d, j);
          r.noiseVolume.create(d, *r.shaders); // no-op without caps
      },
      [](WorldRenderer& r, rhi::Device& d) {
          r.mistMap.destroy(d);
          r.noiseVolume.destroy(d);
      },
      nullptr, nullptr },
    { "occlusion", &Mounted::occlusion,
      [](WorldRenderer& r, rhi::Device&, core::JobSystem& j) {
          r.occlusion.create(j);
      },
      nullptr, nullptr, nullptr },
    { "gi", &Mounted::gi,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem& j) {
          r.radianceCascades.create(d, *r.shaders, j);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.radianceCascades.destroy(d); },
      [](WorldRenderer& r, rhi::Device& d) {
          r.radianceCascades.refreshPipelines(d, *r.shaders);
      },
      nullptr },
    { "grass", &Mounted::grass,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem& j) {
          r.grass.create(d, *r.shaders, j);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.grass.destroy(d); },
      [](WorldRenderer& r, rhi::Device& d) {
          r.grass.refreshPipeline(d, *r.shaders);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.grass.regenerate(d); } },
    // Rock variants: the scene overrides every rock slot with its
    // scanned meshes (LandscapeScene); the procedural placeholder only
    // shows in tool scenes that scatter no rocks.
    { "vegetation", &Mounted::vegetation,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem& j) {
          r.vegetation.create(d, *r.shaders, j, r.terrain.params.seed);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.vegetation.destroy(d); },
      [](WorldRenderer& r, rhi::Device& d) {
          r.vegetation.refreshPipeline(d, *r.shaders);
      },
      [](WorldRenderer& r, rhi::Device& d) {
          r.vegetation.regenerate(d, r.terrain.params.seed);
      } },
    { "sky", &Mounted::sky,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem&) {
          r.sky.create(d, *r.shaders);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.sky.destroy(d); },
      [](WorldRenderer& r, rhi::Device& d) {
          r.sky.refreshPipeline(d, *r.shaders);
      },
      nullptr },
    { "shadows", &Mounted::shadows,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem&) {
          r.shadows.create(d);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.shadows.destroy(d); },
      nullptr, nullptr },
    { "fx", &Mounted::fx,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem&) {
          r.fx.create(d, *r.shaders); // the particle pass
      },
      [](WorldRenderer& r, rhi::Device& d) { r.fx.destroy(d); },
      nullptr, nullptr },
    { "water", &Mounted::water,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem& j) {
          r.water.create(d, *r.shaders, j);
          r.reflectionUbo = { d, d.createBuffer(
              { .usage = rhi::BufferUsage::Uniform,
                .size = sizeof(render::FrameUniforms),
                .dynamic = true },
              nullptr) };
          r.reflectionBindGroup = { d, d.createBindGroup(
              { .entries = { { .binding = 0,
                               .buffer = r.reflectionUbo } } }) };
      },
      [](WorldRenderer& r, rhi::Device& d) {
          r.water.destroy(d);
          r.reflectionBindGroup.reset();
          r.reflectionUbo.reset();
      },
      [](WorldRenderer& r, rhi::Device& d) {
          r.water.refreshPipeline(d, *r.shaders);
      },
      nullptr },
    { "ssdm", &Mounted::offscreen,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem&) {
          r.ssdm.create(d, *r.shaders);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.ssdm.destroy(d); },
      nullptr, nullptr },
    { "postFx", &Mounted::postFx,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem&) {
          r.postFx.create(d, *r.shaders);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.postFx.destroy(d); },
      [](WorldRenderer& r, rhi::Device& d) {
          r.postFx.refreshPipelines(d, *r.shaders);
      },
      nullptr },
    { "gpuOcclusion", &Mounted::gpuOcclusion,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem&) {
          r.gpuOcclusion.create(d, *r.shaders);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.gpuOcclusion.destroy(d); },
      [](WorldRenderer& r, rhi::Device& d) {
          r.gpuOcclusion.refreshPipelines(d, *r.shaders);
      },
      nullptr },
    { "clusters", &Mounted::clusters,
      [](WorldRenderer& r, rhi::Device& d, core::JobSystem&) {
          r.lightClusters.createPipeline(d, *r.shaders);
      },
      [](WorldRenderer& r, rhi::Device& d) { r.lightClusters.destroy(d); },
      [](WorldRenderer& r, rhi::Device& d) {
          r.lightClusters.refreshPipeline(d, *r.shaders);
      },
      nullptr },
};

void WorldRenderer::create(rhi::Device& device, core::JobSystem& jobs,
                               const RendererConfig& config) {
    cfg = config;
    jobSystem = &jobs;
    if (!cfg.postFx) {
        cfg.froxels = false; // froxel fog lives in the postFx chain
    }
    mounted = resolveMounted(cfg, device.caps());
    // The fog knob follows the mount on every create (PostFx::destroy keeps
    // the other tuning knobs across a re-create; this one is a mount fact).
    postFx.froxelFog = mounted.froxels;
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
    if (mounted.clusters) {
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

    // The always-on core: meshes + skinned NPCs + key shadows + the
    // tonemap composite. (The mesh/texture residency caches stay
    // SCENE-owned — the editor and streaming share them; the view hands
    // them in per frame.)
    const u32 white = 0xFFFFFFFF;
    whiteTexture = { device, device.createTexture(
        { .width = 1, .height = 1, .format = rhi::TextureFormat::SRGBA8 },
        &white) };
    meshSampler = { device, device.createSampler({}) };
    shaders->load("mesh",
                  { render::kFrameUbo, { "ModelUbo", 1 },
                    render::kLightsUbo },
                  { { "uAlbedo", 0 } });
    // The depth-only caster variants (sun cascades).
    shaders->load("shadow_mesh",
                  { render::kShadowUbo, { "CasterModelUbo", 4 } });
    shaders->load("shadow_skinned",
                  { render::kShadowUbo, { "CasterModelUbo", 4 } });
    // Placed water surfaces.
    shaders->load("watervolume",
                  { render::kFrameUbo, { "WaterVolumeUbo", 1 } });
    // (No volumetric cumulonimbus pass — cost over look. The cloud MAP
    // keeps the sky alive; stormFront still drives rain/wetness via
    // stormInfo.y.)

    // Rain — procedural streaks (no buffers) + the top-down
    // occlusion depth so roofs keep the drops out.
    if (mounted.sky) {
        shaders->load("rain", { render::kFrameUbo },
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
                  { render::kFrameUbo, { "ModelUbo", 1 },
                    render::kLightsUbo },
                  { { "uAlbedo", 0 } });
    if (mounted.offscreen) {
        blitSampler = { device, device.createSampler({}) }; // linear, clamp — identity
        shaders->load(kTonemapShader, { render::kFrameUbo },
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
    }
    if (mounted.sceneCopy) {
        // The depth copy's sampler — Hi-Z, water and postFx read it, so
        // it lives outside the water gate.
        depthSampler = { device, device.createSampler(
            { .minFilter = rhi::FilterMode::Nearest,
              .magFilter = rhi::FilterMode::Nearest }) };
    }

    // The mounted subsystems, in the table's dependency order.
    for (const SystemEntry& system : kSystems) {
        if (system.create && mounted.*(system.mounted)) {
            system.create(*this, device, jobs);
        }
    }
}

void WorldRenderer::destroy(rhi::Device& device) {
    // Every GPU handle here is an rhi::Unique (WatchedPipeline included),
    // so reset() frees it through its device and nothing is destroyed by
    // hand. The resets below are still required: they force the frees to
    // happen NOW, while the device is alive (the Unique lifetime
    // contract), not at the WorldRenderer destructor. Every Unique member
    // must be reset here, in destroyOffscreenTarget() or by its system's
    // destroy step.
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
    waterMeshSlots.clear();
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
    meshSampler.reset();
    whiteTexture.reset();
    // The subsystems in reverse creation order — unconditionally: a
    // system never created frees nothing, and destroy() must not
    // depend on `mounted` (destroy before create is legal).
    for (size_t i = std::size(kSystems); i-- > 0;) {
        if (kSystems[i].destroy) {
            kSystems[i].destroy(*this, device);
        }
    }
    depthSampler.reset();
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
        appliedReflectionScale == tuning.reflectionScale &&
        appliedSsdmMode == tuning.ssdmMode) {
        return;
    }
    appliedSsdmMode = tuning.ssdmMode;
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
    if (mounted.sceneCopy) {
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
        if (mounted.water) {
            // The reflection resolution is a knob (docs/RENDERING.md) —
            // 0.5 = half res, 0.25 = quarter res (blurrier mirror).
            const u32 reflectionWidth = glm::max(
                static_cast<u32>(static_cast<f32>(width) *
                                 tuning.reflectionScale),
                1u);
            const u32 reflectionHeight = glm::max(
                static_cast<u32>(static_cast<f32>(height) *
                                 tuning.reflectionScale),
                1u);
            appliedReflectionScale = tuning.reflectionScale;
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

    if (mounted.postFx) {
        postFx.resize(device, width, height, offscreenColor, sceneColorCopy,
                      sceneDepthCopy);
    }

    // After postFx.resize: the water shader reprojects the sky-cloud
    // display buffer into the mirror, so the group must reference the
    // freshly (re)created texture.
    if (mounted.water && reflectionColor.id() != 0) {
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
    // The SSDM chain follows the scene target (its targets at the mode's
    // resolution, its groups over the fresh scene copies).
    ssdm.resize(device, tuning.ssdmMode, width, height, sceneColorCopy,
                sceneDepthCopy, blitSampler);
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
    ssdm.destroyTargets(device);
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

void WorldRenderer::drawSceneMeshes(engine::FrameContext& frame,
                                        const render::RenderSnapshot& snapshot,
                                        const RenderView& view,
                                        const render::Frustum* cull) {
    if (snapshot.meshes.empty()) {
        return;
    }
    meshPipeline.ensure(frame.device, *shaders, [&] {
        return frame.device.createPipeline(
            { .shader = shaders->get("mesh"),
              .vertexBuffers = { render::meshVertexLayout() },
              .depth = render::kDepthOpaque,
              .cull = rhi::CullMode::Back });
    });
    // meshDraws is indexed by SNAPSHOT POSITION, and the caster pass
    // recorded into these UBOs earlier this frame under the same
    // indexing: any reorder/filter of snapshot.meshes between the two
    // passes would silently shadow one mesh with another's matrix.
    ENGINE_ASSERT(casterMeshesData == nullptr ||
                  casterMeshesData == snapshot.meshes.data());
    casterMeshesData = nullptr;
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
        if (!instanceVisible(cull, instance.transform,
                             view.meshCache->cpuMesh(instance.model))) {
            continue; // its UBO refreshes the frame it comes back
        }

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
    ensureSkinnedPipeline(frame.device);
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
                  .size = sizeof(ModelUniforms),
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

void WorldRenderer::ensureSkinnedPipeline(rhi::Device& device) {
    skinnedPipeline.ensure(device, *shaders, [&] {
        return device.createPipeline(
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
              .depth = render::kDepthOpaque,
              .cull = rhi::CullMode::Back });
    });
}

f32 WorldRenderer::effectiveWaterSurfaceY(
    const render::RenderSnapshot& snapshot, const RenderView& view) const {
    // The water surface the CAMERA sits under, if any — sea
    // level outdoors, a volume's top when inside one (any worldspace),
    // "dry" otherwise. Feeds the tonemap submersion.
    f32 surface = view.interiorMode ? -1.0e6f : terrain.params.seaLevel;
    const Vec3 eye = view.camera.position;
    // Local lakes/rivers count too: without this, diving under an
    // altitude lake showed no submersion tint at all. R3: the unified
    // query — the sim's dryness is authoritative inside its rect, so a
    // gallery under a baked water claim no longer tints the screen.
    if (!view.interiorMode) {
        const bool simLive = water.simIsValid() && !water.simIsSettling();
        const render::terrain::WaterQuery query {
            simLive ? water.simSnapshot().get() : nullptr,
            water.currentBodies(),
            terrain.params.seaLevel,
        };
        const auto local = render::terrain::waterSurfaceQuery(
            query, eye.x, eye.z, eye.y);
        if (local) {
            surface = glm::max(surface, *local);
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
    waterVolumePipeline.ensure(frame.device, *shaders, [&] {
        return frame.device.createPipeline(
            { .shader = shaders->get("watervolume"),
              .vertexBuffers =
                  { { .stride = 3 * sizeof(f32),
                      .attributes = { { .location = 0,
                                        .format = rhi::VertexFormat::F32x3,
                                        .offset = 0 } } } },
              .blend = rhi::BlendMode::Alpha,
              .depth = render::kDepthReadOnly,
              .cull = rhi::CullMode::None });
    });
    for (WaterQuad& quad : waterQuads) {
        quad.seen = false;
    }
    for (WaterMeshSlot& slot : waterMeshSlots) {
        slot.seen = false;
    }
    bool any = false;
    const auto bindPipeline = [&] {
        if (!any) {
            frame.cmd.setPipeline(waterVolumePipeline);
            frame.cmd.setBindGroup(0, frameBindGroup);
            any = true;
        }
    };
    // The per-draw UBO: tint + chop, then the mesh flag (a transient
    // body shades with its geometric normal, a quad with the flat one).
    struct VolumeUbo {
        Vec4 tint;
        Vec4 meshInfo;
    };
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
                  .size = sizeof(VolumeUbo),
                  .dynamic = true },
                nullptr) };
            slot->group = { frame.device, frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = slot->ubo } } }) };
            const VolumeUbo ubo { Vec4 { volume.tint, volume.chop },
                                  Vec4 { 0.0f } };
            frame.device.updateBuffer(slot->ubo, &ubo, sizeof(ubo), 0);
        }
        bindPipeline();
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
    for (const render::RenderSnapshot::WaterMeshInstance& mesh :
         snapshot.waterMeshes) {
        if (mesh.triangles.size() < 3) {
            continue;
        }
        WaterMeshSlot* slot = nullptr;
        for (WaterMeshSlot& candidate : waterMeshSlots) {
            if (candidate.id == mesh.id) {
                slot = &candidate;
                break;
            }
        }
        if (!slot) {
            waterMeshSlots.push_back({ mesh.id });
            slot = &waterMeshSlots.back();
        }
        slot->seen = true;
        const u64 bytes = mesh.triangles.size() * sizeof(Vec3);
        if (slot->vertices.id() == 0 || slot->capacityBytes < bytes) {
            // Grow geometrically: a jet's segment count is stable, a
            // blob's too — this fires once or twice per gesture.
            slot->capacityBytes = glm::max(bytes * 2, u64 { 4096 });
            slot->vertices = { frame.device, frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = slot->capacityBytes,
                  .dynamic = true },
                nullptr) };
        }
        frame.device.updateBuffer(slot->vertices, mesh.triangles.data(),
                                  bytes, 0);
        slot->vertexCount = static_cast<u32>(mesh.triangles.size());
        if (slot->ubo.id() == 0) {
            slot->ubo = { frame.device, frame.device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  .size = sizeof(VolumeUbo),
                  .dynamic = true },
                nullptr) };
            slot->group = { frame.device, frame.device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = slot->ubo } } }) };
        }
        const VolumeUbo ubo { Vec4 { mesh.tint, mesh.chop },
                              Vec4 { 1.0f, 0.0f, 0.0f, 0.0f } };
        frame.device.updateBuffer(slot->ubo, &ubo, sizeof(ubo), 0);
        bindPipeline();
        frame.cmd.setBindGroup(1, slot->group);
        frame.cmd.setVertexBuffer(0, slot->vertices);
        frame.cmd.draw(slot->vertexCount);
    }
    for (auto it = waterMeshSlots.begin(); it != waterMeshSlots.end();) {
        if (!it->seen) {
            it = waterMeshSlots.erase(it);
        } else {
            ++it;
        }
    }
}

void WorldRenderer::ensureCasterPipelines(rhi::Device& device) {
    // Position-only attributes over the FULL vertex strides (same buffers
    // as the lit pass); depth state mirrors terrain/vegetation casters.
    meshCasterPipeline.ensure(device, *shaders, [&] {
        return device.createPipeline(
            { .shader = shaders->get("shadow_mesh"),
              .vertexBuffers = { render::meshVertexPositionLayout() },
              .depth = render::kDepthCaster,
              .cull = rhi::CullMode::Back });
    });
    skinnedCasterPipeline.ensure(device, *shaders, [&] {
        return device.createPipeline(
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
              .depth = render::kDepthCaster,
              .cull = rhi::CullMode::Back });
    });
}

void WorldRenderer::drawShadowCasters(
    engine::FrameContext& frame, const render::RenderSnapshot& snapshot,
    const RenderView& view, u32 cascade, const render::Frustum* cull) {
    drawCastersInto(frame, snapshot, view, shadows.casterBindGroup(cascade),
                    cascade == 0, cull);
}

void WorldRenderer::drawCastersInto(engine::FrameContext& frame,
                                        const render::RenderSnapshot& snapshot,
                                        const RenderView& view,
                                        rhi::BindGroupHandle casterGroup,
                                        bool refreshUbos,
                                        const render::Frustum* cull) {
    ensureCasterPipelines(frame.device);
    const bool firstCascade = refreshUbos;

    // Scene meshes: the per-draw UBO is the lit pass's — the caster pass
    // runs first in the frame, so cascade 0 refreshes the model matrix
    // (drawSceneMeshes rewrites the full block later the same frame).
    // This shared-UBO scheme only works while BOTH passes see the SAME
    // meshes at the SAME indices — stamped here, asserted by the lit pass.
    if (!snapshot.meshes.empty()) {
        casterMeshesData = snapshot.meshes.data();
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
                      .size = sizeof(ModelUniforms),
                      .dynamic = true },
                    nullptr) };
            }
            // The matrix refresh stays UNCONDITIONAL: later cascades and
            // the key tiles reuse this UBO and cull against a DIFFERENT
            // volume — a mesh culled here may still draw there.
            if (firstCascade) {
                frame.device.updateBuffer(draw.ubo, &instance.transform,
                                          sizeof(Mat4), 0);
            }
            if (!instanceVisible(cull, instance.transform,
                                 view.meshCache->cpuMesh(instance.model))) {
                continue;
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
            if (cull &&
                !cull->intersectsSphere(
                    Vec3 { instance.transform[3] } +
                        Vec3 { 0.0f, 1.0f, 0.0f },
                    kSkinnedCasterRadius)) {
                continue;
            }
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

void WorldRenderer::recordPostFx(engine::FrameContext& frame, const RenderView& view,
                               const render::FrameUniforms& frameData,
                               bool useOffscreen) {
    // Bloom pyramid + god rays + volumetric shafts, composed by the tonemap.
    if (mounted.postFx && useOffscreen) {
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
        // SSDM scatter: fresh color copy (the pre-water one was
        // consumed), then the chain resolves back into the offscreen
        // target.
        if (tuning.ssdmMode != render::SsdmChain::kOff && useOffscreen &&
            mounted.sceneCopy && ssdm.ready() &&
            sceneColorCopy.id() != 0) {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "ssdm" };
            frame.cmd.copyTexture(offscreenColor, sceneColorCopy);
            ssdm.record(frame.device, frame.cmd, *shaders, tuning.ssdmMode,
                        offscreenFb, frameBindGroup);
        }
        // Contact shadows (the texture is the toggle — white = off).
        {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "contact" };
            if (tuning.contactShadows && !view.interiorMode) {
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
            if (tuning.ssao) {
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
        if (tuning.autoExposure) {
            render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                          "autoExpo" };
            postFx.renderAutoExposure(frame.device, frame.cmd,
                                      frameBindGroup);
        }
    }
}

void WorldRenderer::recordCopyHizWater(engine::FrameContext& frame,
                                     const RenderView& view,
                                     const Mat4& viewProj, bool useOffscreen) {
    const render::Camera3D& camera = view.camera;
    // Snapshot the opaque scene (sampling a bound attachment is UB): the
    // SSAO pass reads the depth copy EVERY frame — interiors included
    // (skipping it left the previous exterior's AO ghosting over the
    // room). Water composition and Hi-Z occlusion stay exterior-only.
    if (useOffscreen && mounted.sceneCopy && sceneColorCopy.id() != 0) {
        core::FrameProbe::Scope probe { *view.probe, "copyHizWater" };
        render::GpuProbe::Scope gpu { gpuProbe, frame.device,
                                      "copyHizWater" };
        frame.cmd.copyTexture(offscreenColor, sceneColorCopy);
        frame.cmd.copyTexture(offscreenDepth, sceneDepthCopy);

        // GPU Hi-Z occlusion: pyramid from this frame's depth
        // snapshot + cull dispatch; the verdict is read back NEXT frame
        // (CPU path) or consumed as indirect commands (GPU-driven path).
        occlusionCommandsFresh = false;
        if (mounted.gpuOcclusion && !view.interiorMode) {
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
            if (mounted.vegetation) {
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

        if (mounted.water && !view.interiorMode &&
            waterSceneBindGroup.id() != 0) {
            frame.cmd.beginRenderPass({ .framebuffer = offscreenFb,
                                        .loadOp = rhi::LoadOp::Load,
                                        .depthLoadOp = rhi::LoadOp::Load });
            water.draw(frame.cmd, frameBindGroup, waterSceneBindGroup);
            frame.cmd.endRenderPass();
        }
    }
}

void WorldRenderer::recordMainPass(engine::FrameContext& frame,
                                 const render::RenderSnapshot& snapshot,
                                 const RenderView& view,
                                 const render::Frustum& viewFrustum,
                                 const render::FrameUniforms& frameData,
                                 bool useOffscreen) {
    const render::Camera3D& camera = view.camera;
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
        if (fireScorch.bindGroup().id != 0) {
            frame.cmd.setBindGroup(8, fireScorch.bindGroup());
        }
        if (noiseVolume.ready() && noiseVolume.fireBindGroup().id != 0) {
            frame.cmd.setBindGroup(9, noiseVolume.fireBindGroup());
        }
        // Occlusion applies to the main view only: the set is built for
        // the real camera, not the mirrored one (the grass ring is too
        // close to ever be ridge-occluded — frustum only). CPU horizon
        // only — the GPU verdict drives the indirect commands directly.
        combinedOccluded.clear();
        if (tuning.occlusion && occlusion.occludedSet()) {
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
            if (mounted.terrain && tuning.farTerrain) {
                // Far silhouettes FIRST, the horizon ring first of all:
                // everything nearer overdraws them by depth; they
                // extend the world past the ring.
                for (u32 ring = kFarRingCount; ring-- > 0;) {
                    if (farRingEnabled(ring)) {
                        farRings[ring].draw(frame.cmd, frameBindGroup,
                                            sky.cloudMapBindGroup());
                    }
                }
            }
            // GPU-driven path (docs/RENDERING.md §6.0): consume the
            // indirect commands the cull dispatch wrote LAST frame — the
            // verdict never crossed the CPU. Falls back to the per-chunk
            // loops whenever the commands aren't fresh (interiors, first
            // frames, toggle off, candidate overflow).
            const bool indirectDraw =
                tuning.gpuIndirect && tuning.gpuOcclusion &&
                frame.device.caps().multiDrawIndirect &&
                occlusionCommandsFresh && gpuOcclusion.commandsValid();
            if (mounted.terrain) {
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
            if (mounted.vegetation) {
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
            if (mounted.grass) {
                render::GpuProbe::Scope sub { subProbe, subDevice,
                                              "mainGrass" };
                grass.draw(frame.cmd, frameBindGroup,
                           shadows.receiverBindGroup(), camera.position,
                           &viewFrustum);
            }
        }
        drawSceneMeshes(frame, snapshot, view,
                        &viewFrustum); // RenderSnapshot.meshes
        drawSkinned(frame, snapshot);        // the Forms-driven skinned NPCs
        if (mounted.sky && !view.interiorMode) {
            sky.draw(frame.cmd, frameBindGroup); // background only
        }
        // Placed water surfaces (alpha) after every opaque.
        if (mounted.water) {
            drawWaterVolumes(frame, snapshot);
        }
        // The frame's particles (camera-facing quads; the
        // extract sorted the alpha batch, additive is order-free).
        rhi::TextureHandle flameSheet {};
        if (snapshot.flameSheet.texture != core::Guid {} &&
            view.materialTextures) {
            flameSheet = view.materialTextures->resolve(snapshot.flameSheet.texture);
        }
        fx.draw(frame, *shaders, frameBindGroup, snapshot.fxAlpha,
                snapshot.fxAdditive, snapshot.fxFlames, snapshot.fxHaze,
                flameSheet, waterSceneBindGroup);
        // Rain streaks (procedural, camera cylinder).
        if (mounted.sky && frameData.stormInfo.y > 0.003f) {
            rainPipeline.ensure(frame.device, *shaders, [&] {
                return frame.device.createPipeline(
                    { .shader = shaders->get("rain"),
                      .blend = rhi::BlendMode::Alpha,
                      .depth = render::kDepthReadOnly,
                      .cull = rhi::CullMode::None });
            });
            frame.cmd.setPipeline(rainPipeline);
            frame.cmd.setBindGroup(0, frameBindGroup);
            frame.cmd.setBindGroup(1, rainReceiverGroup);
            frame.cmd.draw(3000 * 6);
        }
        frame.cmd.endRenderPass();
    }
}

void WorldRenderer::recordReflection(engine::FrameContext& frame, const RenderView& view,
                                   const render::FrameUniforms& uniforms,
                                   bool reflectionsActive) {
    const render::Camera3D& camera = view.camera;
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
        if (fireScorch.bindGroup().id != 0) {
            frame.cmd.setBindGroup(8, fireScorch.bindGroup());
        }
        if (noiseVolume.ready() && noiseVolume.fireBindGroup().id != 0) {
            frame.cmd.setBindGroup(9, noiseVolume.fireBindGroup());
        }
        if (mounted.terrain) {
            terrain.draw(frame.cmd, reflectionBindGroup,
                         shadows.receiverBindGroup(), &reflectionFrustum);
            // Far silhouettes in the mirror (inner ring only — the
            // horizon ring at 24 km adds nothing to a wobbly half-res
            // reflection): without them a lake at dusk mirrored SKY
            // where the eye sees mountains.
            if (farRingEnabled(kInnerRing)) {
                farRings[kInnerRing].draw(frame.cmd, reflectionBindGroup,
                                          sky.cloudMapBindGroup());
            }
        }
        // Trees only: rocks and bushes are invisible in a wobbly half-res
        // reflection — low-detail canopies for the same reason.
        if (mounted.vegetation) {
            vegetation.draw(frame.cmd, reflectionBindGroup,
                            shadows.receiverBindGroup(),
                            render::VegetationSystem::kTreeVariants,
                            camera.position, /*forceLowDetail=*/true,
                            &reflectionFrustum);
        }
        if (mounted.sky) {
            sky.draw(frame.cmd, reflectionBindGroup);
        }
        frame.cmd.endRenderPass();
    }
}

void WorldRenderer::recordShadowCascades(engine::FrameContext& frame,
                                       const render::RenderSnapshot& snapshot,
                                       const RenderView& view,
                                       const render::ShadowMapper::Cascades& cascades,
                                       const array<bool, render::ShadowMapper::kCascadeCount>& cascadeDue,
                                       f32 shadowStrength) {
    const render::Camera3D& camera = view.camera;
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
            if (mounted.terrain) {
                terrain.drawDepth(frame.cmd, shadows.casterBindGroup(i),
                                  camera.position, casterChunks,
                                  &cascadeFrustum);
            }
            // Same 13-chunk cap: the last cascade ends at 800 m (the
            // ultra tree ring). Far cascades cast with the solid shadow
            // proxies (metaball blobs), cascade 0 with the leafy cards.
            if (mounted.vegetation) {
                if (fireScorch.bindGroup().id != 0) {
                    // The caster drops the burnt canopies too (slot 8,
                    // bound per pass: the frame's replay starts empty).
                    frame.cmd.setBindGroup(8, fireScorch.bindGroup());
                }
                vegetation.drawDepth(frame.cmd, frameBindGroup,
                                     shadows.casterBindGroup(i),
                                     camera.position, casterChunks,
                                     &cascadeFrustum,
                                     /*ultraDetail=*/i > 0);
            }
            // Scene meshes + NPCs join the casters (A/B toggle).
            if (tuning.meshShadowCasters) {
                drawShadowCasters(frame, snapshot, view, i,
                                  &cascadeFrustum);
            }
            frame.cmd.endRenderPass();
        }
    }
}

void WorldRenderer::recordRainOcclusion(engine::FrameContext& frame,
                                      const render::RenderSnapshot& snapshot,
                                      const RenderView& view,
                                      const render::FrameUniforms& frameData) {
    const render::Camera3D& camera = view.camera;
    // The top-down rain occlusion depth (roof + canopy cover).
    if (mounted.sky && frameData.stormInfo.y > 0.003f &&
        tuning.meshShadowCasters) {
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "rainOcc" };
        frame.cmd.beginRenderPass({ .framebuffer = rainOcclusionFb,
                                    .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::Clear });
        const render::Frustum rainFrustum = render::Frustum::fromViewProj(
            frameData.rainOcclusionViewProj);
        drawCastersInto(frame, snapshot, view, rainCasterGroup,
                        /*refreshUbos=*/true, &rainFrustum);
        // Trees shelter too: the vegetation caster path through the
        // top-down matrix. Billboards orient to THEIR pass's matrix
        // (shadow_prop.vert reads the bound ShadowUbo), so cards face up
        // here; solid shadow proxies / ultra lobes keep it cutout-free.
        // The window is 40 m around the camera — one chunk of reach.
        if (mounted.vegetation) {
            if (fireScorch.bindGroup().id != 0) {
                frame.cmd.setBindGroup(8, fireScorch.bindGroup());
            }
            vegetation.drawDepth(frame.cmd, frameBindGroup,
                                 rainCasterGroup, camera.position,
                                 /*maxChunkDistance=*/1, nullptr,
                                 /*ultraDetail=*/true);
        }
        frame.cmd.endRenderPass();
    }
}

void WorldRenderer::recordKeyShadowTiles(engine::FrameContext& frame,
                                       const render::RenderSnapshot& snapshot,
                                       const RenderView& view,
                                       const vector<KeyShadowPick>& keyShadowPicks) {
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
            const render::Frustum keyFrustum = render::Frustum::fromViewProj(
                keyShadowPicks[slot].viewProj);
            drawCastersInto(frame, snapshot, view,
                            keyShadowCasterGroups[slot],
                            /*refreshUbos=*/slot == 0, &keyFrustum);
        }
        frame.cmd.endRenderPass();
    }
}

void WorldRenderer::pumpStreaming(engine::FrameContext& frame, const RenderView& view) {
    if (!view.interiorMode) { // interiors: no terrain/scatter/water to stream
        if (mounted.terrain && tuning.sharedHeightField) {
            // The shared height pyramid publishes FIRST so this frame's
            // consumer bakes capture the freshest snapshot.
            heightField.update(terrain.params, view.camera.position);
        }
        if (mounted.terrain) {
            core::FrameProbe::Scope probe { *view.probe, "terrain" };
            terrain.update(frame.device, view.camera.position,
                           streamingHold, streamingBoost);
        }
        if (mounted.terrain) {
            // Probed apart from the chunk streaming — a landing bake
            // recreates the 256² map (upload) and must be attributable.
            core::FrameProbe::Scope probe { *view.probe, "lightmap" };
            // Pump/kick the light-map bake (worker; re-bakes on
            // the quantized sun step or when the focus strays).
            // With the pyramid enabled, hold the FIRST kick until a
            // snapshot exists (~2 s of boot): a bake racing ahead of it
            // runs the exact path — the very 2-minute bake the grid
            // exists to remove.
            const auto hf = heightFieldSnapshot();
            if (!tuning.sharedHeightField || hf) {
                terrainLightMap.update(frame.device, terrain.params,
                                       view.camera.position,
                                       shadowSunDirection, hf);
            }
            // Region shading maps (biome/wetness fields for the splat
            // rules; sun-independent, re-bakes on stray or terrain
            // republish).
            terrainShadeMap.update(frame.device, terrain.params,
                                   view.camera.position);
        }
        if (mounted.mist) {
            // Valley data for the ground mist (sun-independent; re-bakes
            // only when the camera strays half a kilometer).
            core::FrameProbe::Scope probe { *view.probe, "mistmap" };
            mistMap.update(frame.device, terrain.params,
                           view.camera.position, heightFieldSnapshot());
        }
        if (mounted.terrain && tuning.farTerrain) {
            // Distant silhouettes: the two worker-baked rings
            // (18 km + 57.6 km horizon), fed the same inputs. Only the
            // inner ring scatters impostors.
            core::FrameProbe::Scope probe { *view.probe, "farterrain" };
            farRings[kInnerRing].impostorsFromGrid =
                tuning.farImpostorsFromGrid;
            const FarRingInputs in {
                .trees = mounted.vegetation
                             ? vegetation.treeSilhouette()
                             : render::VegetationSystem::TreeSilhouette {},
                .layerAlbedos = { terrain.layerAlbedoBase(0),
                                  terrain.layerAlbedoBase(1),
                                  terrain.layerAlbedoBase(2),
                                  terrain.layerAlbedoBase(3),
                                  terrain.layerAlbedoBase(4) },
                .field = heightFieldSnapshot(),
            };
            for (u32 ring = 0; ring < kFarRingCount; ++ring) {
                if (farRingEnabled(ring)) {
                    farRings[ring].update(frame.device, terrain.params,
                                          view.camera.position, in.trees,
                                          in.layerAlbedos, in.field);
                }
            }
        }
        // Height-horizon occlusion: rebuilt on a worker
        // whenever the camera strays; stays valid (conservative) meanwhile.
        if (mounted.occlusion) {
            core::FrameProbe::Scope probe { *view.probe, "occlusion" };
            occlusion.pump();
            occlusion.configure(static_cast<f32>(terrain.viewRadius) *
                                render::TerrainSystem::kChunkSize);
            if (occlusion.wantsRebuild(view.camera.position)) {
                occlusion.rebuild(terrain.params, view.camera.position,
                                  terrain.chunkTops(),
                                  tuning.sharedHeightField
                                      ? heightField.snapshot()
                                      : nullptr);
            }
        }
        if (mounted.gi) {
            // The GI's albedo tile bounces the same tinted ground. The
            // strength settles first: a panel drag used to re-kick the
            // 65k-texel tile bake on every tick past 0.001 of delta.
            radianceCascades.terrainTintStrength =
                giTintSettle.settled(view.terrainTintStrength, frame.dt);
        }
        if (mounted.grass) {
            core::FrameProbe::Scope probe { *view.probe, "grass" };
            // Root-albedo bake follows the terrain's splat scale,
            // macro-tint strength and the ACTIVE material set's variant
            // means (one ground-color source, cooked A/B included). The
            // knobs settle first: a slider drag re-scattered the whole
            // 49-chunk grass ring on every tick — now once, at its end.
            std::array<f32, 14> liveKnobs {};
            liveKnobs[0] = view.splatUvScale;
            liveKnobs[1] = view.terrainTintStrength;
            for (u32 v = 0; v < 4; ++v) {
                const Vec3 base = terrain.grassAlbedoBase(v);
                liveKnobs[2 + v * 3 + 0] = base.x;
                liveKnobs[2 + v * 3 + 1] = base.y;
                liveKnobs[2 + v * 3 + 2] = base.z;
            }
            const std::array<f32, 14>& s =
                grassKnobSettle.settled(liveKnobs, frame.dt);
            bool changed =
                grass.scatterTuning.splatUvScale != s[0] ||
                grass.scatterTuning.tintStrength != s[1];
            for (u32 v = 0; v < 4; ++v) {
                changed = changed ||
                          grass.scatterTuning.rootAlbedoBase[v] !=
                              Vec3 { s[2 + v * 3 + 0], s[2 + v * 3 + 1],
                                     s[2 + v * 3 + 2] };
            }
            if (changed) {
                grass.scatterTuning.splatUvScale = s[0];
                grass.scatterTuning.tintStrength = s[1];
                for (u32 v = 0; v < 4; ++v) {
                    grass.scatterTuning.rootAlbedoBase[v] =
                        Vec3 { s[2 + v * 3 + 0], s[2 + v * 3 + 1],
                               s[2 + v * 3 + 2] };
                }
                grass.regenerate(frame.device);
            }
            grass.update(frame.device, terrain.params,
                         view.camera.position, streamingHold);
        }
        if (mounted.vegetation) {
            core::FrameProbe::Scope probe { *view.probe, "veg" };
            vegetation.update(frame.device, terrain.params,
                              view.camera.position, streamingHold);
        }
        if (mounted.water) {
            core::FrameProbe::Scope probe { *view.probe, "water" };
            water.setRainIntensity(
                view.interiorMode ? 0.0f : view.atmos.rainIntensity);
            water.update(frame.device, terrain.params,
                         view.camera.position, heightFieldSnapshot());
            water.updateSim(frame.device, terrain.params,
                            view.camera.position, frame.dt);
        }
    }
}

void WorldRenderer::pumpPipelinesAndRequests(engine::FrameContext& frame) {
    shaders->pollHotReload(frame.dt);
    for (const SystemEntry& system : kSystems) {
        if (system.refresh && mounted.*(system.mounted)) {
            system.refresh(*this, frame.device);
        }
    }
    if (mounted.terrain) {
        terrain.setWireframe(tuning.wireframe, frame.device, *shaders);
    }
    if (regenerateRequested) {
        regenerateRequested = false;
        for (const SystemEntry& system : kSystems) {
            if (system.regenerate && mounted.*(system.mounted)) {
                system.regenerate(*this, frame.device);
            }
        }
        occlusion.invalidate();
    }
    // A/B mesh-only swap at the safe point — instance buffers and
    // scatter stay resident.
    if (reseedVegetation) {
        reseedVegetation = false;
        if (mounted.vegetation) {
            vegetation.reseedVariantMeshes(frame.device);
        }
    }
    // Grass panel: a scatter knob moved — re-scatter the meadow only.
    if (grassRescatterRequested) {
        grassRescatterRequested = false;
        if (mounted.grass) {
            grass.regenerate(frame.device);
        }
    }
    // CSM resolution knob: recreate on change; the
    // round-robin then re-renders every cascade next frames (the fresh
    // maps start empty — one frame of unshadowed sun at worst).
    if (static_cast<u32>(tuning.shadowResolution) != shadows.resolution()) {
        shadows.destroy(frame.device);
        shadows.create(frame.device,
                       static_cast<u32>(tuning.shadowResolution));
        lastCascadesValid = false; // force a full cascade re-render
    }
    // Terrain sculpt: re-mesh JUST the chunks a stroke touched (in place, no
    // hole) — runs live during the stroke for real-time feedback. Grass/veg
    // re-scatter only on commit (`sculptScatterChunks`) so they don't flicker
    // every preview frame. The rest of the world stays put.
    if (mounted.terrain && !sculptDirtyChunks.empty()) {
        terrain.remeshChunks(sculptDirtyChunks);
        sculptDirtyChunks.clear();
    }
    if (!sculptScatterChunks.empty()) {
        if (mounted.grass) {
            grass.invalidateChunks(frame.device, sculptScatterChunks);
        }
        if (mounted.vegetation) {
            vegetation.invalidateChunks(frame.device, sculptScatterChunks);
        }
        sculptScatterChunks.clear();
    }
}

void WorldRenderer::render(engine::FrameContext& frame,
                               const render::RenderSnapshot& snapshot,
                               const RenderView& view) {
    // Resolve last frames' timestamps (never blocking) and
    // open this frame's slot — the scopes below feed the budget table.
    gpuProbe.beginFrame(frame.device);
    logBudgetOnce();
    pumpPipelinesAndRequests(frame);
    pumpStreaming(frame, view);

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
        (tuning.shadows && shadowsAvailable && !view.interiorMode)
            ? glm::smoothstep(-0.02f, 0.06f, skyState.sunDirection.y) *
                  (1.0f - 0.65f * view.atmos.cloudCoverage)
            : 0.0f;
    render::ShadowMapper::Cascades cascades {};
    array<bool, render::ShadowMapper::kCascadeCount> cascadeDue {
        true, true, true
    };
    fitShadowCascades(frame, camera, skyState.sunDirection, shadowStrength,
                      cascades, cascadeDue);
    const bool reflectionsActive = reflectionWanted(view, viewFrustum);

    // GI: decide this frame's inject + snap the grid origins
    // BEFORE the UBO composition so uGiGridInfo matches the volume
    // content the apply will sample (the moving-halo lag fix).
    if (mounted.gi) {
        radianceCascades.prepare(camera.position);
    }

    // The whole UBO composition is pure: gather the inputs,
    // let the composer build both variants, upload. This side keeps only
    // the GPU-availability gates and the updateBuffer calls.
    const ComposedFrame composed = composeFrameUniforms(
        composerInputs(frame, snapshot, view, viewProj, skyState, cascades,
                       shadowStrength, reflectionsActive));
    const render::FrameUniforms& uniforms = composed.base;
    render::FrameUniforms frameData = composed.resolved;
    // Clustered forward (docs/RENDERING.md §5): the grid's far reach is
    // the froxel slicing's (interior room scale / exterior CSM reach) so
    // both grids share their z slices by construction.
    const bool clustered = tuning.clusteredLights && lightClusters.ready();
    frameData.clusterInfo = { clustered ? 1.0f : 0.0f,
                              render::volumetricReach(view.interiorMode,
                                                      view.atmos.fogStart),
                              0.0f, 0.0f };
    if (mounted.sky && frameData.stormInfo.y > 0.003f) {
        frame.device.updateBuffer(rainOcclusionUbo,
                                  &frameData.rainOcclusionViewProj,
                                  sizeof(Mat4), 0);
    }

    // Key-shadow atlas selection (docs/RENDERING.md §5 B6): the up-to-4
    // best-scored castsShadow lights get a 1024² tile. Selected HERE so
    // the tile matrices ride this frame-UBO upload and the per-light
    // slot lands in the lights UBO below (windowInfo.z). The tiles
    // themselves render after the cloud bake.
    vector<KeyShadowPick> keyShadowPicks;
    if (tuning.keyShadow && tuning.meshShadowCasters) {
        keyShadowPicks = pickKeyShadows(snapshot.shadowLights, camera.position,
                                        shadowSunDirection, kKeyShadowSlots);
        for (size_t slot = 0; slot < keyShadowPicks.size(); ++slot) {
            frameData.keyShadowAtlasViewProj[slot] =
                keyShadowPicks[slot].viewProj;
        }
    }
    frame.device.updateBuffer(frameUbo, &frameData, sizeof(frameData), 0);

    // The selected local lights. Only the clustered path may consume
    // the full budget; the legacy per-pixel loop stays capped at
    // kFallbackLights (its cost is per-slot x per-pixel). With "lights
    // via RC only" the lights exist purely in the GI volume: their
    // occlusion and penumbras come from the cascades, at voxel
    // resolution.
    {
        const LightsUniforms lights = fillLightsUbo({
            .lights = snapshot.lights,
            .budget = clustered ? kMaxLights : kFallbackLights,
            .rcOnly = radianceCascades.tuning.rcOnlyLights &&
                      radianceCascades.tuning.technique ==
                          render::GiTechnique::RadianceCascades,
            .shadowSunDirection = shadowSunDirection,
            .sunColor = Vec3 { uniforms.sunColor },
            .buriedBelowY = view.buriedBelowY,
            .timeSeconds = view.timeSeconds,
            .keyShadowPicks = keyShadowPicks,
        });
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
    if (mounted.sky && !view.interiorMode) {
        render::GpuProbe::Scope gpu { gpuProbe, frame.device, "cloudBake" };
        sky.bakeCloudMap(frame.cmd, frameBindGroup);
    }

    recordKeyShadowTiles(frame, snapshot, view, keyShadowPicks);

    recordRainOcclusion(frame, snapshot, view, frameData);

    recordShadowCascades(frame, snapshot, view, cascades, cascadeDue,
                         shadowStrength);

    // GI voxel clipmap re-injection (docs/RENDERING.md) — its
    // HISTORICAL slot: after the CSM passes (the inject samples fresh
    // shadow maps), outside any render pass. Pipelined (PG2), the chain
    // records at the END of the frame instead and this slot only fences
    // LAST frame's cascade 0 for this frame's consumers — everything
    // recorded above (cluster cull, cloud bake, CSM, key shadows) reads
    // no GI and overlaps the still-running chain.
    if (mounted.gi) {
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

    const bool useOffscreen = mounted.offscreen;
    if (useOffscreen) {
        ensureOffscreenTarget(frame.device, frame.width, frame.height);
        blitPipeline.ensure(frame.device, *shaders, [&] {
            return frame.device.createPipeline(
                { .shader = shaders->get(kTonemapShader) });
        });
    }

    recordReflection(frame, view, uniforms, reflectionsActive);

    // (The Hi-Z verdict readback is gone — docs/RENDERING.md §6.0 I6:
    // the cull's verdict lives in the indirect commands and never
    // crosses the CPU. The legacy draw path keeps the CPU horizon
    // occlusion only.)

    recordMainPass(frame, snapshot, view, viewFrustum, frameData,
                   useOffscreen);

    recordCopyHizWater(frame, view, viewProj, useOffscreen);

    recordPostFx(frame, view, frameData, useOffscreen);

    // Pipelined GI (docs/RENDERING.md PG2): the chain records HERE, at the
    // end of the frame — this frame consumed LAST frame's cascade 0; the
    // chain overlaps the composite, the present and the next frame's
    // front, up to the consumer fence before the next reflection.
    if (mounted.gi && radianceCascades.tuning.pipelined) {
        recordGiUpdate(frame, snapshot, view, uniforms, clustered);
    }

    recordComposite(frame, view, useOffscreen);
}

FrameComposerInputs WorldRenderer::composerInputs(
    engine::FrameContext& frame, const render::RenderSnapshot& snapshot,
    const RenderView& view, const Mat4& viewProj,
    const render::SkySystem::SkyState& skyState,
    const render::ShadowMapper::Cascades& cascades, f32 shadowStrength,
    bool reflectionsActive) {
    const render::Camera3D& camera = view.camera;
    return {
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
        .interiorDaylightWeight =
            tuning.interiorDaylightWeight *
            aboveBuried(view.camera.position.y, view.buriedBelowY),
        .froxelFog =
            mounted.froxels && postFx.froxelFog && postFx.froxelReady(),
        .interiorDustDensity = tuning.interiorDustDensity,
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
        // AO/sheen read the cooked ORM: the procedural fallback binds
        // a placeholder there, so the knobs must go dark with it.
        .surfAoStrength =
            terrain.hasCookedMaterials() ? view.surfAoStrength : 0.0f,
        .wetDarken = view.wetDarken,
        .wetSheen = terrain.hasCookedMaterials() ? view.wetSheen : 0.0f,
        .snowSheen = terrain.hasCookedMaterials() ? view.snowSheen : 0.0f,
        .barkEnabled = vegetation.barkLoaded(),
        .ssaoStrength = tuning.ssaoStrength,
        .ssaoRadius = tuning.ssaoRadius,
        .ssdmAmplitude = tuning.ssdmMode != 0 ? tuning.ssdmAmp : 0.0f,
        .shadowFarUvScale = render::ShadowMapper::kFarCascadeScale,
        .reflectionsActive = reflectionsActive,
        // Horizon closure: the active horizon (far mesh reach or the
        // streaming ring), NEVER past the camera's farPlane — the fog
        // must finish dissolving before the clip plane cuts (a closure
        // beyond the farPlane leaves a hard geometry edge against the
        // sky). z of the same uniform carries the ring for the far
        // mesh's sink bias.
        .drawDistance =
            mounted.terrain ? glm::min(horizonReach(),
                                       view.camera.farPlane * 0.95f)
                            : 0.0f,
        .nearRingDistance = mounted.terrain
                                ? static_cast<f32>(terrain.viewRadius) *
                                      render::TerrainSystem::kChunkSize
                                : 0.0f,
        .treeFadeEnd = vegetation.treeFadeEnd(),
        .seasonAutumn = tuning.seasonAutumn,
        .seasonLeafFall = tuning.seasonLeafFall,
        .leafSeason = vegetation.leafSeason(),
        .debugBuffer = tuning.debugBuffer,
        .stylized = tuning.stylized,
        .tonemap = tuning.tonemap,
        .exposure = tuning.exposure,
        .cascadeDebug = tuning.cascadeDebug,
        .grading = tuning.grading,
        .gradeVibrance = tuning.gradeVibrance,
        .gradeSplitTone = tuning.gradeSplitTone,
        .gradeContrast = tuning.gradeContrast,
        .autoExposure = tuning.autoExposure,
        .autoExposureMin = tuning.autoExposureMin,
        .autoExposureMax = tuning.autoExposureMax,
        .waterMapInfo = water.poolMapInfo(),
        .terrainLightInfo = terrainLightMap.info(),
        .terrainLightActive =
            tuning.terrainLight && !view.interiorMode && terrainLightMap.ready(),
        .waterSurfaceY = effectiveWaterSurfaceY(snapshot, view),
        .windTime = view.windTime,
        .windDrift = view.windDrift,
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
        .stylizedDiffuseInfo = tuning.stylizedDiffuse,
        .stylizedShadowInfo = tuning.stylizedShadow,
        .stylizedSpecInfo = tuning.stylizedSpec,
        // giInfo() gates on ready() itself (interiors included).
        .giInfo = radianceCascades.giInfo(),
        .giGridInfo = radianceCascades.giGridInfo(),
        .giBandInfo = { radianceCascades.tuning.bandCount,
                        radianceCascades.tuning.bandAa,
                        radianceCascades.tuning.giFloor, 0.0f },
        .mistActive = tuning.mist && mistMap.ready(),
        .mistCoverageSoftness = tuning.mistCoverageSoftness,
        .mistReach = tuning.mistReach,
        .fogHorizonTail = tuning.fogHorizonTail,
        // The sea reaches the farthest enabled ring (phantom-land
        // guard); the inner ring's span when none is drawn.
        .seaQuadHalfSpan =
            kFarRingConfigs[farRingEnabled(kHorizonRing) ? kHorizonRing
                                                         : kInnerRing]
                .span *
            0.5f,
        .mistShapeInfo = tuning.mistShape,
        .mistMapInfo = mistMap.info(),
        .mistDetailInfo = { noiseVolume.ready() && tuning.mistNoiseTex ? 1.0f
                                                                  : 0.0f,
                            static_cast<f32>(tuning.mistSteps),
                            tuning.mistDetailDropout, tuning.mistSunBoost },
        .mistLightInfo = tuning.mistLight,
        .cloudVolInfo = { tuning.skyClouds && noiseVolume.ready() ? 1.0f : 0.0f,
                          tuning.skyCloudShape.x, tuning.skyCloudShape.y,
                          tuning.skyCloudShape.z },
        .cloudVolLightInfo = tuning.skyCloudLight,
        .cloudVolShapeInfo = { tuning.skyCloudShape.w, tuning.skyCloudLiningLobe,
                               tuning.skyCloudPowder, tuning.skyCloudPuffiness },
        .cloudVolRimInfo = { tuning.skyCloudRimGain, tuning.skyCloudRimLobe,
                             tuning.skyCloudBaseDark, 0.0f },
        .mistPuffInfo = { tuning.mistPuffiness, 0.0f, 0.0f, 0.0f },
        .waterDebugInfo = { static_cast<f32>(tuning.waterDebug),
                            water.simConfig().pinRiverHalfWidth,
                            0.0f, 0.0f },
        .waterSimMapInfo = water.simMapInfo(),
        .waterSimTuneInfo = water.simTuneInfo(),
        .waterSimFrozen = water.simFrozenInfo(),
        .terrainShadeMapInfo = terrainShadeMap.info(),
        .fireScorchInfo = fireScorch.info(),
        .fireEmberInfo = { fireScorch.look.bedIntensity, fireScorch.look.pulse,
                           fireScorch.look.erode,
                           noiseVolume.ready() ? 1.0f : 0.0f },
        .fireEmberHot = { fireScorch.look.emberHot, fireScorch.look.coalScale },
        .fireEmberCold = { fireScorch.look.emberCold, fireScorch.look.ash },
        .fireCharInfo = { fireScorch.look.charColor, fireScorch.look.grassCull },
        .fireFlameInfo = { snapshot.flameSheet.columns, snapshot.flameSheet.rows,
                           snapshot.flameSheet.fps, snapshot.flameSheet.aspect },
        .fireFlameLook = { fireScorch.look.flameBoost,
                           fireScorch.look.flamePosterize,
                           fireScorch.look.hazeStrength, 0.0f },
    };
}

void WorldRenderer::logBudgetOnce() {
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
    // Worker-job telemetry: fold landed costs every frame; mirror the
    // one-shot budget line for headless/scripted sessions.
    if (jobSystem) {
        core::JobProbe& probe = jobSystem->probe();
        probe.drain();
        if (perfFrames == 2000 && !probe.rows().empty()) {
            str line;
            char cell[96];
            for (const core::JobProbe::Row& row : probe.rows()) {
                std::snprintf(cell, sizeof(cell), " | %s %ux avg %.0fms",
                              row.name, row.runs, row.averageMs());
                line += cell;
            }
            LOG_INFO("cpu bakes (window): {:.1f} s total{}",
                     probe.windowTotalMs() / 1000.0, line);
        }
    }
}

void WorldRenderer::fitShadowCascades(
    engine::FrameContext& frame, const render::Camera3D& camera,
    const Vec3& sunDirection, f32 shadowStrength,
    render::ShadowMapper::Cascades& cascades,
    array<bool, render::ShadowMapper::kCascadeCount>& cascadeDue) {
    // The cascades use a QUANTIZED sun (otherwise tree shadows tremble).
    // The texel snap absorbs camera translation, but the game clock spins
    // the light a fraction of a degree every frame, re-basing the snap —
    // the edges crawl. Hysteresis instead: shadows sit rock-stable, then
    // take an imperceptible ~0.4° step every ~8 real seconds; the VISIBLE
    // sun/lighting keeps moving smoothly.
    bool sunStepped = false;
    if (glm::dot(shadowSunDirection, sunDirection) <
        std::cos(glm::radians(0.4f))) {
        shadowSunDirection = sunDirection;
        sunStepped = true;
    }
    // Which cascades actually re-render this frame (docs/RENDERING.md):
    // most of the shadow cost is the two
    // far cascades. Round-robin: cascade 0 every frame, cascades 1 and 2
    // on alternate frames, each keeping its PREVIOUS matrix when skipped
    // (the stale depth must be sampled with the matrix it was drawn
    // with). A sun step (or the A/B toggle off) re-renders all three.
    if (shadowStrength > 0.0f) {
        cascades = shadows.computeCascades(camera, frame.aspect,
                                           shadowSunDirection);
        ++shadowFrame;
        const bool full =
            !tuning.shadowRoundRobin || !lastCascadesValid || sunStepped;
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
}

bool WorldRenderer::reflectionWanted(const RenderView& view,
                                     const render::Frustum& viewFrustum) {
    // Planar reflection is meaningful only from above the surface —
    // and (auto-skip) only when some water can actually show: a
    // RESIDENT chunk dipping below sea level inside the frustum.
    bool waterVisible = true;
    if (mounted.water && tuning.reflectionAutoSkip && !view.interiorMode &&
        tuning.reflections) {
        terrain.collectChunkAabbs(occlusionAabbs);
        waterVisible = seaVisibleInFrustum(occlusionAabbs,
                                           terrain.params.seaLevel,
                                           view.camera.position, viewFrustum);
    }
    return tuning.reflections && reflectionFb.id() != 0 &&
           !view.interiorMode &&
           view.camera.position.y > terrain.params.seaLevel && waterVisible;
}

void WorldRenderer::recordComposite(engine::FrameContext& frame,
                                    const RenderView& view,
                                    bool useOffscreen) {
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
        if (mounted.gi) {
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
            Vec3 lo;
            Vec3 hi;
            instanceWorldBounds(mesh.transform, *cpu, lo, hi);
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
        if (mounted.vegetation && !view.interiorMode) {
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
        // Sun-linked emitters carry the live sun into the GI field too —
        // same color/gate as the direct path above.
        const f32 sunGate =
            glm::smoothstep(0.05f, 0.20f, shadowSunDirection.y);
        for (const auto& light : snapshot.lights) {
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
