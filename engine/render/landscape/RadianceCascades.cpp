#include "engine/render/landscape/RadianceCascades.hpp"

#include <glm/glm.hpp>

#include "engine/core/Jobs.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"

namespace render {

namespace {

constexpr const char* kInjectShader = "rc_inject";
constexpr const char* kBuildShader = "rc_build";
constexpr const char* kMergeShader = "rc_merge";
constexpr const char* kAdaptShader = "rc_adapt";
constexpr const char* kDebugShader = "rc_debug";
constexpr const char* kFullscreenVert = "fullscreen";

// Flat proxy albedos per terrain material — GI only needs the broad hue
// of what the light bounces off (the real splat tiles never leave the
// terrain shader). Rough matches of the splat family averages.
// (Grass slightly brighter than the raw splat average — the visible
// green bounce was too subtle, dev report 2026-07-11.)
constexpr Vec3 kGrassAlbedo { 0.090f, 0.155f, 0.052f };
constexpr Vec3 kRockAlbedo { 0.180f, 0.165f, 0.150f };
constexpr Vec3 kSandAlbedo { 0.420f, 0.360f, 0.250f };
constexpr Vec3 kSnowAlbedo { 0.620f, 0.660f, 0.720f };

// The std140 mirror of the RcUbo block in rc_inject.comp / rc_debug.frag.
struct RcUniforms {
    Vec4 clipInfo[2]; // xyz = min-corner origin, w = voxel size
    Vec4 tileInfo;    // xy = tile center XZ, z = 1/span, w = resolution
    Vec4 misc;        // x = debug clip index, y = sky factor,
                      // z = box count, w = light count (G3)
    Vec4 lightPosRadius[RadianceCascades::kMaxLights]; // G3
    Vec4 lightColor[RadianceCascades::kMaxLights];
    Vec4 misc2; // APPENDED (adaptive ramp): x = band count,
                // y = contrast floor (log2 stops), z = adapt speed
};

// std140 mirror of RcCascadeUbo (rc_build.comp / rc_merge.comp).
struct RcCascadeUniforms {
    Vec4 a; // x = cascade index, y = probes/axis, z/w = dir grid W/H
    Vec4 b; // build: x = interval0, y = march step, z = spacing, w = dirMajor
            // merge: x = interval0, y = sky flag,   z = spacing, w = dirMajor
};

} // namespace

void RadianceCascades::create(rhi::Device& device, ShaderLibrary& shaders,
                              core::JobSystem& jobSystem) {
    if (!device.caps().volumeTextures || !device.caps().computeShaders) {
        LOG_WARN("RadianceCascades: volume textures / compute unavailable "
                 "— GI disabled");
        return;
    }
    jobs = &jobSystem;
    baked = std::make_shared<core::ConcurrentQueue<BakedTile>>();

    shaders.loadCompute(kInjectShader, { { "FrameUbo", 0 }, { "RcUbo", 2 } },
                        { { "uShadowMap", 1 },
                          { "uTerrainLight", 7 },
                          { "uRcHeight", 8 },
                          { "uRcAlbedo", 9 } });
    shaders.loadCompute(kBuildShader,
                        { { "FrameUbo", 0 }, { "RcUbo", 2 },
                          { "RcCascadeUbo", 4 } },
                        { { "uRcClipFineS", 5 }, { "uRcClipCoarseS", 6 } });
    shaders.loadCompute(kMergeShader,
                        { { "FrameUbo", 0 }, { "RcUbo", 2 },
                          { "RcCascadeUbo", 4 } },
                        { { "uRcSrc", 7 } });
    shaders.loadCompute(kAdaptShader, { { "RcUbo", 2 } },
                        { { "uRcCascade0", 10 } });
    shaders.load(kDebugShader, { { "FrameUbo", 0 }, { "RcUbo", 2 } },
                 { { "uRcClipFine", 5 }, { "uRcClipCoarse", 6 },
                   { "uRcCascade0", 10 } },
                 kFullscreenVert);

    tileSampler = { device, device.createSampler(
        { .minFilter = rhi::FilterMode::Linear,
          .magFilter = rhi::FilterMode::Linear }) };
    volumeSampler = { device, device.createSampler(
        { .minFilter = rhi::FilterMode::Linear,
          .magFilter = rhi::FilterMode::Linear }) };

    makePlaceholderTile(device);

    rcUbo = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform, .size = sizeof(RcUniforms),
          .dynamic = true }, nullptr) };
    boxBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Storage,
          .size = kMaxBoxes * sizeof(RcBox), .dynamic = true }, nullptr) };
    cascadeUbo = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Uniform,
          .size = sizeof(RcCascadeUniforms), .dynamic = true }, nullptr) };
    // Adaptive-ramp stats (GPU-written, never read back): zero-init so
    // the reduce SNAPS on its first run (w = 0 sentinel).
    const Vec4 zeroStats { 0.0f };
    statsBuffer = { device, device.createBuffer(
        { .usage = rhi::BufferUsage::Storage, .size = sizeof(Vec4) },
        &zeroStats) };

    refreshPipelines(device, shaders);
}

void RadianceCascades::destroy(rhi::Device& device) {
    (void)device; // Unique handles free through their device
    adaptPipeline.reset();
    mergePipeline.reset();
    buildPipeline.reset();
    debugPipeline.reset();
    injectPipeline.reset();
    adaptGroup.reset();
    applyGroup_.reset();
    statsBuffer.reset();
    levels.clear();
    debugGroup.reset();
    injectGroup.reset();
    cascadeUbo.reset();
    boxBuffer.reset();
    rcUbo.reset();
    clipCoarse.reset();
    clipFine.reset();
    albedoTex.reset();
    heightTex.reset();
    volumeSampler.reset();
    tileSampler.reset();
    baked.reset();
    jobs = nullptr;
    tileUploaded = false;
    tileInFlight = false;
    appliedResolution = 0;
}

void RadianceCascades::refreshPipelines(rhi::Device& device,
                                        ShaderLibrary& shaders) {
    if (!jobs) {
        return; // create() bailed (no caps)
    }
    if (shaders.generation(kInjectShader) != injectGeneration) {
        injectPipeline = { device, device.createComputePipeline(
            { shaders.get(kInjectShader) }) };
        injectGeneration = shaders.generation(kInjectShader);
    }
    if (shaders.generation(kBuildShader) != buildGeneration) {
        buildPipeline = { device, device.createComputePipeline(
            { shaders.get(kBuildShader) }) };
        buildGeneration = shaders.generation(kBuildShader);
    }
    if (shaders.generation(kMergeShader) != mergeGeneration) {
        mergePipeline = { device, device.createComputePipeline(
            { shaders.get(kMergeShader) }) };
        mergeGeneration = shaders.generation(kMergeShader);
    }
    if (shaders.generation(kAdaptShader) != adaptGeneration) {
        adaptPipeline = { device, device.createComputePipeline(
            { shaders.get(kAdaptShader) }) };
        adaptGeneration = shaders.generation(kAdaptShader);
    }
    if (shaders.generation(kDebugShader) != debugGeneration) {
        debugPipeline = { device, device.createPipeline(
            { .shader = shaders.get(kDebugShader),
              .blend = rhi::BlendMode::Alpha,
              .depth = { .testEnable = false, .writeEnable = false },
              .cull = rhi::CullMode::None }) };
        debugGeneration = shaders.generation(kDebugShader);
    }
}

void RadianceCascades::makePlaceholderTile(rhi::Device& device) {
    // "No terrain": height far below any voxel — INTERIORS run on this
    // permanently (kit boxes + lights carry the room, dev feedback
    // 2026-07-11); exteriors swap in the worker bake via pumpTileBake.
    const f32 kNoTerrain = -1.0e4f;
    heightTex = { device, device.createTexture(
        { .width = 1, .height = 1, .format = rhi::TextureFormat::R16F },
        &kNoTerrain) };
    const u32 kGray = 0xFF808080;
    albedoTex = { device, device.createTexture(
        { .width = 1, .height = 1, .format = rhi::TextureFormat::RGBA8 },
        &kGray) };
    tileSpan = 1.0f;
    tileCenter = Vec2 { 0.0f };
    tileUploaded = true;
    tileIsPlaceholder = true;
    appliedResolution = 0; // bind groups reference the NEW tile textures
}

void RadianceCascades::createVolumes(rhi::Device& device) {
    const u32 res = static_cast<u32>(glm::clamp(tuning.resolution, 16, 128));
    clipFine = { device, device.createTexture(
        { .width = res, .height = res, .depth = res,
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Linear }, nullptr) };
    clipCoarse = { device, device.createTexture(
        { .width = res, .height = res, .depth = res,
          .format = rhi::TextureFormat::RGBA16F,
          .filter = rhi::FilterMode::Linear }, nullptr) };

    injectGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 2, .buffer = rcUbo.get() },
                       { .binding = 3, .buffer = boxBuffer.get(),
                         .storage = true },
                       { .binding = 0, .texture = clipFine.get(),
                         .storageImage = true },
                       { .binding = 1, .texture = clipCoarse.get(),
                         .storageImage = true },
                       { .binding = 8, .texture = heightTex.get(),
                         .sampler = tileSampler.get() },
                       { .binding = 9, .texture = albedoTex.get(),
                         .sampler = tileSampler.get() } } }) };
    // --- Cascade levels (G4/G5) ------------------------------------------
    // Count clamped so the top level keeps >= 2 probes per axis, and the
    // dir-major cascade 0 stays under common GL_MAX_3D_TEXTURE_SIZE.
    i32 count = glm::clamp(tuning.cascadeCount, 1, 8);
    while (count > 1 && (static_cast<i32>(res) >> (count - 1)) < 2) {
        --count;
    }
    levels.clear();
    levels.resize(static_cast<size_t>(count));
    for (i32 i = 0; i < count; ++i) {
        CascadeLevel& level = levels[static_cast<size_t>(i)];
        level.probes = glm::max(res >> i, 2u);
        level.dirsW = 4u << i; // octahedral grid: 4×2 dirs at c0, ×2/axis
        level.dirsH = 2u << i;
        if (i == 0) { // dir-major slabs (hardware trilinear for the apply)
            level.width = level.probes;
            level.height = level.probes;
            level.depth = level.probes * level.dirsW * level.dirsH;
        } else { // dir-tiled
            level.width = level.probes * level.dirsW;
            level.height = level.probes * level.dirsH;
            level.depth = level.probes;
        }
        level.texture = { device, device.createTexture(
            { .width = level.width, .height = level.height,
              .depth = level.depth,
              .format = rhi::TextureFormat::RGBA16F,
              .filter = rhi::FilterMode::Linear }, nullptr) };
        level.buildGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 2, .buffer = rcUbo.get() },
                           { .binding = 4, .buffer = cascadeUbo.get() },
                           { .binding = 0, .texture = level.texture.get(),
                             .storageImage = true },
                           { .binding = 5, .texture = clipFine.get(),
                             .sampler = volumeSampler.get() },
                           { .binding = 6, .texture = clipCoarse.get(),
                             .sampler = volumeSampler.get() } } }) };
    }
    for (i32 i = 0; i < count; ++i) {
        CascadeLevel& level = levels[static_cast<size_t>(i)];
        // Merge src = level i+1 (already merged); the TOP merges the sky
        // (flag in the ubo) — its sampler slot gets a dummy (never read).
        const rhi::TextureHandle src =
            i + 1 < count ? levels[static_cast<size_t>(i) + 1].texture.get()
                          : clipFine.get();
        level.mergeGroup = { device, device.createBindGroup(
            { .entries = { { .binding = 2, .buffer = rcUbo.get() },
                           { .binding = 4, .buffer = cascadeUbo.get() },
                           { .binding = 0, .texture = level.texture.get(),
                             .storageImage = true },
                           { .binding = 7, .texture = src,
                             .sampler = volumeSampler.get() } } }) };
    }
    appliedCascadeCount = tuning.cascadeCount;

    debugGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 2, .buffer = rcUbo.get() },
                       { .binding = 5, .texture = clipFine.get(),
                         .sampler = volumeSampler.get() },
                       { .binding = 6, .texture = clipCoarse.get(),
                         .sampler = volumeSampler.get() },
                       { .binding = 10, .texture = levels[0].texture.get(),
                         .sampler = volumeSampler.get() } } }) };
    // G6: the surface shaders' sampler (gi.glsl, binding 11) + the
    // adaptive-ramp stats SSBO (binding 12).
    applyGroup_ = { device, device.createBindGroup(
        { .entries = { { .binding = 11, .texture = levels[0].texture.get(),
                         .sampler = volumeSampler.get() },
                       { .binding = 12, .buffer = statsBuffer.get(),
                         .storage = true } } }) };
    // The adaptive-ramp reduction: strided samples of merged cascade 0.
    adaptGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 2, .buffer = rcUbo.get() },
                       { .binding = 12, .buffer = statsBuffer.get(),
                         .storage = true },
                       { .binding = 10, .texture = levels[0].texture.get(),
                         .sampler = volumeSampler.get() } } }) };

    appliedResolution = tuning.resolution;
    appliedFineVoxel = tuning.fineVoxel;
    appliedCoarseVoxel = tuning.coarseVoxel;
    // (A coarse-span change re-kicks the tile bake through pumpTileBake's
    // own spanChanged check — the current tile keeps serving meanwhile.)
}

void RadianceCascades::pumpTileBake(rhi::Device& device,
                                    const TerrainParams& params,
                                    const Vec3& cameraPos) {
    // Upload a finished bake. R16F accepts tightly packed f32 uploads (the
    // GL converts) — plenty for terrain heights; textures are recreated
    // with initial pixels (no updateTexture in the RHI), and the bind
    // group is rebuilt below through the appliedResolution reset.
    BakedTile tile;
    while (baked->tryPop(tile)) {
        if (tile.gen != tileGeneration) {
            continue; // stale (params/knobs changed since the kick)
        }
        heightTex = { device, device.createTexture(
            { .width = kTileSize, .height = kTileSize,
              .format = rhi::TextureFormat::R16F },
            tile.height.data()) };
        albedoTex = { device, device.createTexture(
            { .width = kTileSize, .height = kTileSize,
              .format = rhi::TextureFormat::RGBA8 },
            tile.albedo.data()) };
        // The bind group references the OLD textures — rebuild it.
        appliedResolution = 0; // forces createVolumes' group rebuild below
        tileCenter = tile.center;
        tileInFlight = false;
        tileUploaded = true;
        tileIsPlaceholder = false;
    }

    // Kick a re-bake when the camera strays from the tile center.
    const f32 span =
        static_cast<f32>(tuning.resolution) * tuning.coarseVoxel * 1.25f;
    const Vec2 focus { cameraPos.x, cameraPos.z };
    const bool spanChanged = glm::abs(span - tileSpan) > 0.01f;
    if (!tileInFlight &&
        (spanChanged ||
         glm::distance(focus, tileCenter) > span * 0.10f)) {
        tileSpan = span;
        tileInFlight = true;
        const u64 gen = ++tileGeneration;
        auto queue = baked;
        const TerrainParams paramsCopy = params;
        jobs->enqueue([queue, paramsCopy, focus, span, gen] {
            BakedTile out;
            out.center = focus;
            out.gen = gen;
            out.height.resize(kTileSize * kTileSize);
            out.albedo.resize(kTileSize * kTileSize * 4);
            const f32 texel = span / static_cast<f32>(kTileSize);
            for (u32 ty = 0; ty < kTileSize; ++ty) {
                for (u32 tx = 0; tx < kTileSize; ++tx) {
                    const f32 wx = focus.x +
                                   (static_cast<f32>(tx) + 0.5f -
                                    kTileSize * 0.5f) * texel;
                    const f32 wz = focus.y +
                                   (static_cast<f32>(ty) + 0.5f -
                                    kTileSize * 0.5f) * texel;
                    out.height[ty * kTileSize + tx] =
                        terrain::height(paramsCopy, wx, wz);
                }
            }
            for (u32 ty = 0; ty < kTileSize; ++ty) {
                for (u32 tx = 0; tx < kTileSize; ++tx) {
                    const u32 i = ty * kTileSize + tx;
                    const f32 h = out.height[i];
                    const u32 xn = tx > 0 ? i - 1 : i;
                    const u32 xp = tx < kTileSize - 1 ? i + 1 : i;
                    const u32 zn = ty > 0 ? i - kTileSize : i;
                    const u32 zp = ty < kTileSize - 1 ? i + kTileSize : i;
                    const Vec3 n = glm::normalize(
                        Vec3 { -(out.height[xp] - out.height[xn]) /
                                   (2.0f * texel),
                               1.0f,
                               -(out.height[zp] - out.height[zn]) /
                                   (2.0f * texel) });
                    const terrain::MaterialWeights w =
                        terrain::materialWeights(paramsCopy, h, n);
                    Vec3 albedo = kGrassAlbedo * w.grass +
                                  kRockAlbedo * w.rock +
                                  kSandAlbedo * w.sand +
                                  kSnowAlbedo * w.snow;
                    const f32 sum =
                        glm::max(w.grass + w.rock + w.sand + w.snow, 1e-3f);
                    albedo /= sum;
                    out.albedo[i * 4 + 0] = static_cast<u8>(
                        glm::clamp(albedo.x, 0.0f, 1.0f) * 255.0f);
                    out.albedo[i * 4 + 1] = static_cast<u8>(
                        glm::clamp(albedo.y, 0.0f, 1.0f) * 255.0f);
                    out.albedo[i * 4 + 2] = static_cast<u8>(
                        glm::clamp(albedo.z, 0.0f, 1.0f) * 255.0f);
                    out.albedo[i * 4 + 3] = 255;
                }
            }
            queue->push(std::move(out));
        });
    }
}

void RadianceCascades::prepare(const Vec3& cameraPos) {
    if (appliedResolution <= 0) {
        injectThisFrame = true; // first frames: update() creates volumes
        return;
    }
    ++frameCounter;
    injectThisFrame =
        tuning.updateInterval <= 1 ||
        (frameCounter % static_cast<u32>(tuning.updateInterval)) == 0;
    if (!injectThisFrame) {
        return; // origins keep matching the volumes' last inject
    }
    const u32 res = static_cast<u32>(appliedResolution);
    const auto snap = [&](f32 voxel) {
        const f32 half = static_cast<f32>(res) * voxel * 0.5f;
        return Vec3 { std::floor((cameraPos.x - half) / voxel) * voxel,
                      std::floor((cameraPos.y - half) / voxel) * voxel,
                      std::floor((cameraPos.z - half) / voxel) * voxel };
    };
    lastFineOrigin = snap(appliedFineVoxel);
    lastCoarseOrigin = snap(appliedCoarseVoxel);
}

void RadianceCascades::update(rhi::Device& device, rhi::CommandBuffer& cmd,
                              const TerrainParams& params,
                              const Vec3& cameraPos,
                              const vector<RcBox>& boxes,
                              const vector<RcLight>& lights,
                              bool bakeTerrain,
                              rhi::BindGroupHandle frameBindGroup,
                              rhi::BindGroupHandle shadowBindGroup,
                              rhi::BindGroupHandle terrainLightGroup,
                              rhi::Device* probeDevice, GpuProbe* probe) {
    if (!jobs || injectPipeline.id() == 0) {
        return;
    }
    if (bakeTerrain) {
        pumpTileBake(device, params, cameraPos);
    } else if (!tileIsPlaceholder) {
        // Interior entered with the exterior tile still loaded: its
        // heights belong to another worldspace — back to "no terrain".
        makePlaceholderTile(device);
    }
    bool recreated = false;
    if (appliedResolution != tuning.resolution ||
        appliedFineVoxel != tuning.fineVoxel ||
        appliedCoarseVoxel != tuning.coarseVoxel ||
        appliedCascadeCount != tuning.cascadeCount) {
        createVolumes(device); // knob moved (or tile textures recreated)
        recreated = true;
    }
    if (!injectThisFrame) {
        return; // budget knob: hold last frame's volumes (prepare() kept
                // the origins matching their content)
    }
    if (recreated) {
        // prepare() snapped with the PREVIOUS applied values (boot / knob
        // change): re-snap. The frame UBO is one frame stale here — a
        // single-frame glitch on the knob-change frame, accepted.
        const u32 r = static_cast<u32>(appliedResolution);
        const auto snap = [&](f32 voxel) {
            const f32 half = static_cast<f32>(r) * voxel * 0.5f;
            return Vec3 { std::floor((cameraPos.x - half) / voxel) * voxel,
                          std::floor((cameraPos.y - half) / voxel) * voxel,
                          std::floor((cameraPos.z - half) / voxel) *
                              voxel };
        };
        lastFineOrigin = snap(appliedFineVoxel);
        lastCoarseOrigin = snap(appliedCoarseVoxel);
    }

    const u32 res = static_cast<u32>(appliedResolution);
    RcUniforms uniforms;
    // The origins prepare() snapped BEFORE the frame UBO was composed —
    // uGiGridInfo and the volume content stay in lockstep (dev bug).
    uniforms.clipInfo[0] = { lastFineOrigin, appliedFineVoxel };
    uniforms.clipInfo[1] = { lastCoarseOrigin, appliedCoarseVoxel };
    uniforms.tileInfo = { tileCenter.x, tileCenter.y, 1.0f / tileSpan,
                          static_cast<f32>(res) };
    const u32 boxCount =
        glm::min(static_cast<u32>(boxes.size()), kMaxBoxes);
    const u32 lightCount =
        glm::min(static_cast<u32>(lights.size()), kMaxLights);
    uniforms.misc = { static_cast<f32>(tuning.debugView), tuning.skyFactor,
                      static_cast<f32>(boxCount),
                      static_cast<f32>(lightCount) };
    uniforms.misc2 = { static_cast<f32>(glm::clamp(tuning.bands, 2, 5)),
                       glm::max(tuning.contrastFloor, 0.1f),
                       glm::clamp(tuning.adaptSpeed, 0.005f, 1.0f), 0.0f };
    for (u32 i = 0; i < lightCount; ++i) {
        uniforms.lightPosRadius[i] = lights[i].positionRadius;
        uniforms.lightColor[i] = lights[i].color;
    }
    device.updateBuffer(rcUbo, &uniforms, sizeof(uniforms), 0);
    if (boxCount > 0) {
        device.updateBuffer(boxBuffer, boxes.data(),
                            boxCount * sizeof(RcBox), 0);
    }

    {
        GpuProbe::Scope gpu { probe, probeDevice, "rcInject" };
        cmd.setPipeline(injectPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        if (shadowBindGroup.id != 0) {
            cmd.setBindGroup(2, shadowBindGroup);
        }
        if (terrainLightGroup.id != 0) {
            cmd.setBindGroup(4, terrainLightGroup);
        }
        cmd.setBindGroup(1, injectGroup);
        cmd.dispatch((res + 3) / 4, (res + 3) / 4, (res * 2 + 3) / 4);
        cmd.memoryBarrier(); // clips visible to the cascade builds
    }

    // Per-level dispatch parameters. NOTE: reusing ONE ubo with an
    // updateBuffer between dispatches relies on the GL backend executing
    // immediately — a Vulkan backend would want per-level UBOs or dynamic
    // offsets (documented deviation, revisit with the backend).
    const auto levelUniforms = [&](size_t i, f32 flagB) {
        const CascadeLevel& level = levels[i];
        RcCascadeUniforms cu;
        cu.a = { static_cast<f32>(i), static_cast<f32>(level.probes),
                 static_cast<f32>(level.dirsW),
                 static_cast<f32>(level.dirsH) };
        cu.b = { tuning.interval0, flagB,
                 appliedFineVoxel * static_cast<f32>(1 << i),
                 i == 0 ? 1.0f : 0.0f };
        return cu;
    };

    // G4: build every cascade (each marches the fresh clipmap; levels are
    // independent — one barrier after the batch).
    {
        GpuProbe::Scope gpu { probe, probeDevice, "rcBuild" };
        cmd.setPipeline(buildPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        for (size_t i = 0; i < levels.size(); ++i) {
            const CascadeLevel& level = levels[i];
            // b.y = march step: fine voxels near, coarse for far levels.
            const RcCascadeUniforms cu = levelUniforms(
                i, i <= 1 ? appliedFineVoxel : appliedCoarseVoxel);
            device.updateBuffer(cascadeUbo, &cu, sizeof(cu), 0);
            cmd.setBindGroup(1, level.buildGroup);
            cmd.dispatch((level.width + 3) / 4, (level.height + 3) / 4,
                         (level.depth + 3) / 4);
        }
        cmd.memoryBarrier();
    }

    // G5: merge top -> 0. The top absorbs the SKY; each level below
    // absorbs the already-merged level above (barrier between steps).
    {
        GpuProbe::Scope gpu { probe, probeDevice, "rcMerge" };
        cmd.setPipeline(mergePipeline);
        cmd.setBindGroup(0, frameBindGroup);
        for (i32 i = static_cast<i32>(levels.size()) - 1; i >= 0; --i) {
            const CascadeLevel& level = levels[static_cast<size_t>(i)];
            const bool top = i == static_cast<i32>(levels.size()) - 1;
            const RcCascadeUniforms cu =
                levelUniforms(static_cast<size_t>(i), top ? 1.0f : 0.0f);
            device.updateBuffer(cascadeUbo, &cu, sizeof(cu), 0);
            cmd.setBindGroup(1, level.mergeGroup);
            cmd.dispatch((level.width + 3) / 4, (level.height + 3) / 4,
                         (level.depth + 3) / 4);
            cmd.memoryBarrier(); // the level below reads this one
        }
    }

    // Adaptive ramp: measure the merged cascade 0's irradiance range
    // (log-mean + contrast window, temporal inertia) — gi.glsl anchors
    // its flat bands on it (dev design 2026-07-11).
    if (adaptPipeline.id() != 0) {
        cmd.setPipeline(adaptPipeline);
        cmd.setBindGroup(1, adaptGroup);
        cmd.dispatch(1, 1, 1);
        cmd.memoryBarrier(); // stats visible to the surface shaders
    }
}

void RadianceCascades::drawDebug(rhi::CommandBuffer& cmd,
                                 rhi::BindGroupHandle frameBindGroup) {
    if (tuning.debugView == 0 || debugPipeline.id() == 0 ||
        debugGroup.id() == 0 || !tileUploaded) {
        return;
    }
    cmd.setPipeline(debugPipeline);
    cmd.setBindGroup(0, frameBindGroup);
    cmd.setBindGroup(1, debugGroup);
    cmd.draw(3);
}

} // namespace render
