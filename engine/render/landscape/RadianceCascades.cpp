#include "engine/render/landscape/RadianceCascades.hpp"

#include <cstdlib>

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
constexpr const char* kExtendShader = "rc_extend";
constexpr const char* kDebugShader = "rc_debug";
constexpr const char* kProbeShader = "rc_probe";
constexpr const char* kFullscreenVert = "fullscreen";

// Flat proxy albedos per terrain material — GI only needs the broad hue
// of what the light bounces off (the real splat tiles never leave the
// terrain shader). Rough matches of the splat family averages.
// (Grass slightly brighter than the raw splat average — otherwise the
// visible green bounce is too subtle.)
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
    Vec4 misc3; // x = light emitter boost (blob radiance),
                // y = bounce feedback (0 = single bounce / no prev)
    Vec4 prevGrid; // G7a: xyz = LAST inject's fine origin,
                   // w = its probe spacing — where uRcPrev's content is
};

// std140 mirror of RcCascadeUbo (rc_build/rc_merge/rc_extend.comp).
struct RcCascadeUniforms {
    Vec4 a; // x = cascade index, y = probes/axis, z/w = dir grid W/H
    Vec4 b; // build: x = interval0, y = march step, z = spacing, w = dirMajor
            // merge: x = interval0, y = sky flag,   z = spacing, w = dirMajor
    Vec4 c; // build: x = marched interval fraction (G7c)
            // extend: x = this pass's shift distance (m)
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
                          { "uRcAlbedo", 9 },
                          { "uRcPrev", 10 } });
    shaders.loadCompute(kBuildShader,
                        { { "FrameUbo", 0 }, { "RcUbo", 2 },
                          { "RcCascadeUbo", 4 } },
                        { { "uRcClipFineS", 5 }, { "uRcClipCoarseS", 6 } });
    shaders.loadCompute(kMergeShader,
                        { { "FrameUbo", 0 }, { "RcUbo", 2 },
                          { "RcCascadeUbo", 4 } },
                        { { "uRcSrc", 7 } });
    shaders.loadCompute(kExtendShader,
                        { { "FrameUbo", 0 }, { "RcUbo", 2 },
                          { "RcCascadeUbo", 4 } },
                        { { "uRcSrc", 7 } });
    shaders.load(kDebugShader, { { "FrameUbo", 0 }, { "RcUbo", 2 } },
                 { { "uRcClipFine", 5 }, { "uRcClipCoarse", 6 },
                   { "uRcCascade0", 10 } },
                 kFullscreenVert);
    shaders.loadCompute(kProbeShader, { { "FrameUbo", 0 } },
                        { { "uGiCascade0", 11 } });

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

    refreshPipelines(device, shaders);
}

void RadianceCascades::destroy(rhi::Device& device) {
    (void)device; // Unique handles free through their device
    mergePipeline.reset();
    buildPipeline.reset();
    debugPipeline.reset();
    injectPipeline.reset();
    applyGroup_.reset();
    levels.clear();
    debugGroup.reset();
    injectGroup.reset();
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
            { .shader = shaders.get(kBuildShader),
              .pushConstantSize = sizeof(RcCascadeUniforms) }) };
        buildGeneration = shaders.generation(kBuildShader);
    }
    if (shaders.generation(kMergeShader) != mergeGeneration) {
        mergePipeline = { device, device.createComputePipeline(
            { .shader = shaders.get(kMergeShader),
              .pushConstantSize = sizeof(RcCascadeUniforms) }) };
        mergeGeneration = shaders.generation(kMergeShader);
    }
    if (shaders.generation(kExtendShader) != extendGeneration) {
        extendPipeline = { device, device.createComputePipeline(
            { .shader = shaders.get(kExtendShader),
              .pushConstantSize = sizeof(RcCascadeUniforms) }) };
        extendGeneration = shaders.generation(kExtendShader);
    }
    if (probePipeline.id() == 0) {
        probePipeline = { device, device.createComputePipeline(
            { shaders.get(kProbeShader) }) };
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
    // permanently (kit boxes + lights carry the room);
    // exteriors swap in the worker bake via pumpTileBake.
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
                           { .binding = 0, .texture = level.texture.get(),
                             .storageImage = true },
                           { .binding = 7, .texture = src,
                             .sampler = volumeSampler.get() } } }) };
    }
    // G7c: extension ping-pong. Scratch twins exist only while the knob
    // is on (structural — roughly doubles the cascade memory); which
    // levels actually use them is decided per frame (interval0 is live).
    if (tuning.intervalExtension) {
        for (CascadeLevel& level : levels) {
            level.scratch = { device, device.createTexture(
                { .width = level.width, .height = level.height,
                  .depth = level.depth,
                  .format = rhi::TextureFormat::RGBA16F,
                  .filter = rhi::FilterMode::Linear }, nullptr) };
            level.extendToScratch = { device, device.createBindGroup(
                { .entries = { { .binding = 2, .buffer = rcUbo.get() },
                               { .binding = 0,
                                 .texture = level.scratch.get(),
                                 .storageImage = true },
                               { .binding = 7,
                                 .texture = level.texture.get(),
                                 .sampler = volumeSampler.get() } } }) };
            level.extendToTexture = { device, device.createBindGroup(
                { .entries = { { .binding = 2, .buffer = rcUbo.get() },
                               { .binding = 0,
                                 .texture = level.texture.get(),
                                 .storageImage = true },
                               { .binding = 7,
                                 .texture = level.scratch.get(),
                                 .sampler = volumeSampler.get() } } }) };
        }
    }
    appliedExtension = tuning.intervalExtension;
    appliedCascadeCount = tuning.cascadeCount;

    // The injection — after the levels: uRcPrev (binding 10) is LAST
    // frame's merged cascade 0, the multi-bounce feedback (G7a).
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
                         .sampler = tileSampler.get() },
                       { .binding = 10, .texture = levels[0].texture.get(),
                         .sampler = volumeSampler.get() } } }) };
    havePrev = false; // fresh cascade textures hold garbage until merged

    debugGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 2, .buffer = rcUbo.get() },
                       { .binding = 5, .texture = clipFine.get(),
                         .sampler = volumeSampler.get() },
                       { .binding = 6, .texture = clipCoarse.get(),
                         .sampler = volumeSampler.get() },
                       { .binding = 10, .texture = levels[0].texture.get(),
                         .sampler = volumeSampler.get() } } }) };
    // G6: the surface shaders' sampler (gi.glsl, binding 11).
    applyGroup_ = { device, device.createBindGroup(
        { .entries = { { .binding = 11, .texture = levels[0].texture.get(),
                         .sampler = volumeSampler.get() } } }) };
    // The GI health probe (rc_probe.comp): merged cascade 0 -> readback.
    if (probeBuffer.get().id == 0) {
        probeBuffer = { device, device.createBuffer(
            { .usage = rhi::BufferUsage::Storage,
              .size = 9 * sizeof(Vec4),
              .readback = true },
            nullptr) };
    }
    probeGroup = { device, device.createBindGroup(
        { .entries = { { .binding = 11, .texture = levels[0].texture.get(),
                         .sampler = volumeSampler.get() },
                       { .binding = 3, .buffer = probeBuffer.get(),
                         .storage = true } } }) };

    appliedResolution = tuning.resolution;
    appliedFineVoxel = tuning.fineVoxel;
    appliedCoarseVoxel = tuning.coarseVoxel;
    // (A coarse-span change re-kicks the tile bake through pumpTileBake's
    // own spanChanged check — the current tile keeps serving meanwhile.)
}

void RadianceCascades::pumpTileBake(rhi::Device& device,
                                    const TerrainParams& params,
                                    const Vec3& cameraPos) {
    // Upload a finished bake. R16F initial data is packed f32 per texel —
    // both backends convert (GL via GL_FLOAT upload, Vulkan explicitly);
    // textures are recreated with initial pixels (no updateTexture in the
    // RHI), and the bind group is rebuilt below through the
    // appliedResolution reset.
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
    // Pipeline value trace: one line whenever the APPLY state flips or a
    // live knob moves (throttled) — the ground truth for "does this
    // slider reach the shader" questions, straight from the values the
    // frame UBO will carry.
    {
        const Vec4 info = giInfo();
        const bool active = info.x > 0.5f;
        if (active != lastLoggedActive || ++knobLogThrottle >= 30) {
            knobLogThrottle = 0;
            if (active != lastLoggedActive ||
                tuning.intensity != lastLoggedIntensity ||
                tuning.skyFactor != lastLoggedSkyFactor ||
                tuning.giFloor != lastLoggedFloor) {
                const Vec4 grid = giGridInfo();
                LOG_INFO("RC apply {}: ready={} levels={} uGiInfo=({:.2f}, "
                         "{:.2f}, {:.1f}, {:.0f}) grid=({:.1f}, {:.1f}, "
                         "{:.1f} | {:.2f}) skyFactor={:.2f} floor={:.2f}",
                         active ? "ACTIVE" : "INACTIVE", ready(),
                         levels.size(), info.x, info.y, info.z, info.w,
                         grid.x, grid.y, grid.z, grid.w, tuning.skyFactor,
                         tuning.giFloor);
                lastLoggedActive = active;
                lastLoggedIntensity = tuning.intensity;
                lastLoggedSkyFactor = tuning.skyFactor;
                lastLoggedFloor = tuning.giFloor;
            }
        }
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
        appliedCascadeCount != tuning.cascadeCount ||
        appliedExtension != tuning.intervalExtension) {
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
    // uGiGridInfo and the volume content stay in lockstep.
    uniforms.clipInfo[0] = { lastFineOrigin, appliedFineVoxel };
    uniforms.clipInfo[1] = { lastCoarseOrigin, appliedCoarseVoxel };
    uniforms.tileInfo = { tileCenter.x, tileCenter.y, 1.0f / tileSpan,
                          static_cast<f32>(res) };
    // MEADOWS_GI_PROBE_LIGHT=1: a synthetic magenta emitter at the exact
    // point rc_probe.comp samples — magenta in the boot "GI probe" line
    // proves the light-blob path end to end, headless.
    static const bool kProbeLight =
        std::getenv("MEADOWS_GI_PROBE_LIGHT") != nullptr;
    vector<RcLight> patchedLights;
    const vector<RcLight>* lightsIn = &lights;
    if (kProbeLight) {
        patchedLights = lights;
        const f32 span = static_cast<f32>(res) * appliedFineVoxel;
        const Vec3 center = lastFineOrigin + Vec3 { span * 0.5f };
        patchedLights.push_back({ { center, 8.0f },
                                  { 10.0f, 0.0f, 10.0f, 0.0f } });
        lightsIn = &patchedLights;
    }
    const u32 boxCount =
        glm::min(static_cast<u32>(boxes.size()), kMaxBoxes);
    const u32 lightCount =
        glm::min(static_cast<u32>(lightsIn->size()), kMaxLights);
    uniforms.misc = { static_cast<f32>(tuning.debugView), tuning.skyFactor,
                      static_cast<f32>(boxCount),
                      static_cast<f32>(lightCount) };
    uniforms.misc3 = { glm::max(tuning.emitterBoost, 0.0f),
                       havePrev ? glm::clamp(tuning.bounceFeedback, 0.0f,
                                             0.95f)
                                : 0.0f,
                       0.0f, 0.0f };
    uniforms.prevGrid = { prevFineOrigin, prevFineSpacing };
    for (u32 i = 0; i < lightCount; ++i) {
        uniforms.lightPosRadius[i] = (*lightsIn)[i].positionRadius;
        uniforms.lightColor[i] = (*lightsIn)[i].color;
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
        // Clips visible to the cascade builds — compute only, the raster
        // recorded after this chain owes it nothing yet.
        cmd.memoryBarrier(rhi::BarrierDst_Compute);
    }

    // Per-level dispatch parameters, carried by push constants. They used to
    // be ONE ubo rewritten between dispatches, which only worked because GL
    // executes immediately: on Vulkan every recorded dispatch would have read
    // the LAST level's parameters. Push constants are captured into the
    // command stream, so each dispatch keeps its own.
    const auto levelUniforms = [&](size_t i, f32 flagB) {
        const CascadeLevel& level = levels[i];
        RcCascadeUniforms cu;
        cu.a = { static_cast<f32>(i), static_cast<f32>(level.probes),
                 static_cast<f32>(level.dirsW),
                 static_cast<f32>(level.dirsH) };
        cu.b = { tuning.interval0, flagB,
                 appliedFineVoxel * static_cast<f32>(1 << i),
                 i == 0 ? 1.0f : 0.0f };
        cu.c = Vec4 { 1.0f, 0.0f, 0.0f, 0.0f };
        return cu;
    };
    // G7c eligibility, per frame (interval0 is a live knob): extend the
    // levels whose FULL march would take >= 8 steps.
    const auto extendedLevel = [&](size_t i) {
        if (!appliedExtension || extendPipeline.id() == 0) {
            return false;
        }
        const f32 step = i <= 1 ? appliedFineVoxel : appliedCoarseVoxel;
        const f32 length = tuning.interval0 * static_cast<f32>(1u << i);
        return length / step >= 8.0f;
    };

    // G4: build every cascade (each marches the fresh clipmap; levels are
    // independent — one barrier after the batch). G7c levels march only
    // a QUARTER of their interval; the extension doubles it twice below.
    {
        GpuProbe::Scope gpu { probe, probeDevice, "rcBuild" };
        cmd.setPipeline(buildPipeline);
        cmd.setBindGroup(0, frameBindGroup);
        for (size_t i = 0; i < levels.size(); ++i) {
            const CascadeLevel& level = levels[i];
            // b.y = march step: fine voxels near, coarse for far levels.
            RcCascadeUniforms cu = levelUniforms(
                i, i <= 1 ? appliedFineVoxel : appliedCoarseVoxel);
            cu.c.x = extendedLevel(i) ? 0.25f : 1.0f;
            cmd.setPushConstants(&cu, sizeof(cu));
            cmd.setBindGroup(1, level.buildGroup);
            cmd.dispatch((level.width + 3) / 4, (level.height + 3) / 4,
                         (level.depth + 3) / 4);
        }
        cmd.memoryBarrier(rhi::BarrierDst_Compute); // extend/merge read
    }

    // G7c: shift+merge the short fields with themselves, twice (x4 —
    // texture -> scratch -> texture, barrier between passes).
    {
        GpuProbe::Scope gpu { probe, probeDevice, "rcExtend" };
        bool any = false;
        for (size_t i = 0; i < levels.size(); ++i) {
            if (!extendedLevel(i)) {
                continue;
            }
            if (!any) {
                cmd.setPipeline(extendPipeline);
                cmd.setBindGroup(0, frameBindGroup);
                any = true;
            }
            const CascadeLevel& level = levels[i];
            const f32 length =
                tuning.interval0 * static_cast<f32>(1u << i);
            const f32 shortLen = length * 0.25f;
            for (u32 pass = 0; pass < 2; ++pass) {
                RcCascadeUniforms cu = levelUniforms(i, 0.0f);
                cu.c.x = shortLen * static_cast<f32>(1u << pass);
                cmd.setPushConstants(&cu, sizeof(cu));
                cmd.setBindGroup(1, pass == 0 ? level.extendToScratch
                                              : level.extendToTexture);
                cmd.dispatch((level.width + 3) / 4,
                             (level.height + 3) / 4,
                             (level.depth + 3) / 4);
                // Pass 2 / merge reads this write.
                cmd.memoryBarrier(rhi::BarrierDst_Compute);
            }
        }
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
            cmd.setPushConstants(&cu, sizeof(cu));
            cmd.setBindGroup(1, level.mergeGroup);
            cmd.dispatch((level.width + 3) / 4, (level.height + 3) / 4,
                         (level.depth + 3) / 4);
            // The level below (compute) reads this one; the FINAL merge
            // (cascade 0) is what the surface shaders, the volumetric and
            // the froxel inject consume — fragment joins the scope there.
            cmd.memoryBarrier(i == 0 ? (rhi::BarrierDst_Compute |
                                        rhi::BarrierDst_Fragment)
                                     : rhi::BarrierDst_Compute);
        }
    }

    // G7a: the merged cascade 0 now holds THIS inject's world — next
    // frame's injection may feed it back as the extra bounce.
    prevFineOrigin = lastFineOrigin;
    prevFineSpacing = appliedFineVoxel;
    havePrev = true;

    // GI health probe: a one-shot sample of the merged cascade 0 at the
    // volume center, logged at boot — the objective "does the chain
    // produce radiance" check (all-zero in daylight = build/merge broken).
    if (!probeLogged && probePipeline.id() != 0 && probeGroup.id() != 0) {
        ++probeFrame;
        if (probeFrame == 120) {
            cmd.setPipeline(probePipeline);
            cmd.setBindGroup(0, frameBindGroup);
            cmd.setBindGroup(1, probeGroup);
            cmd.dispatch(1, 1, 1);
            cmd.memoryBarrier();
        } else if (probeFrame == 126) {
            array<Vec4, 9> slabs {};
            device.readBuffer(probeBuffer.get(), slabs.data(),
                              sizeof(slabs));
            Vec3 avg { 0.0f };
            f32 alpha = 0.0f;
            for (u32 i = 0; i < 8; ++i) {
                avg += Vec3 { slabs[i] };
                alpha += slabs[i].w;
            }
            avg /= 8.0f;
            alpha /= 8.0f;
            LOG_INFO("GI probe (merged cascade 0, volume center): "
                     "avg=({:.4f}, {:.4f}, {:.4f}) beta={:.2f} "
                     "up=({:.4f}, {:.4f}, {:.4f}) | giAmbient=({:.4f}, "
                     "{:.4f}, {:.4f}) active={:.0f} (classic sentinel "
                     "0.123)",
                     avg.x, avg.y, avg.z, alpha, slabs[1].x, slabs[1].y,
                     slabs[1].z, slabs[8].x, slabs[8].y, slabs[8].z,
                     slabs[8].w);
            probeLogged = true;
        }
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
