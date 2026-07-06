#include "game/scenes/LandscapeScene.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <imgui.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/VisualForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/Image.hpp"
#include "engine/render/MeshBuilder.hpp"
#include "engine/Engine.hpp"
#include "engine/FrameContext.hpp"
#include "engine/assets/GltfMesh.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Input.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/platform/Window.hpp"
#include "engine/render/landscape/FrameUniforms.hpp"
#include "data/forms/AnimForms.hpp"
#include "data/forms/CoreForms.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "world/scene/AnimBridge.hpp"
#include "world/scene/Spawner.hpp"

namespace game {

namespace {

constexpr const char* kTonemapShader = "tonemap";

// B5.5: stat-space -> world mapping (docs/STATS.md §3; the CombatArena's
// kSpeedScale precedent, recalibrated for meters). Default sheet (~102):
// jog ~5.1 m/s, sprint x1.6 ~8.2 m/s, velocity settles in ~0.1 s.
// (Dev feel pass 2026-07-06: +50% — the unencumbered adventurer is brisk;
// encumbrance will pull it back down when the P1 utility pass lands.)
constexpr f32 kSpeedScale3D = 1.0f / 20.0f; // movementSpeed stat -> m/s
constexpr f32 kSprintMult = 1.6f;           // "sprint multiplies" (STATS.md)
constexpr f32 kAccelRate3D = 0.12f;         // acceleration stat -> 1/s ramp

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

void LandscapeScene::onEnter() {
    rhi::Device& device = engine->getDevice();

    // Load the moddable data (§5): the landscape tuning plugin, then the
    // adventure plugin (props/NPCs of the 3D gameplay socle) on top.
    registerLandscapeFormTypes(formTypes);
    data::registerCoreFormTypes(formTypes);       // ActorForm (the player)
    data::registerVisualFormTypes(formTypes);     // MaterialForm, StaticForm
    data::registerAnimFormTypes(formTypes);       // clips + locomotion graph
    world::registerWorldFormTypes(formTypes);     // ReferenceForm, markers...
    gameplay::registerGameplayFormTypes(formTypes); // EffectForm (sprint...)
    gameplay::registerStatsFormTypes(formTypes);    // StatsTuningForm (mods)
    gameplay::registerCharacterFormTypes(formTypes); // AppearanceForm (NPC)
    const auto landscapePlugin = data::loadPluginFile(
        platform::executableDir() / "data" / "base" / "landscape.toml",
        formTypes);
    const auto adventurePlugin = data::loadPluginFile(
        platform::executableDir() / "data" / "base" / "adventure.toml",
        formTypes);
    forms = data::FormDatabase {};   // fresh on re-enter
    assetDb = assets::AssetDatabase {};
    vector<const data::Plugin*> loadOrder;
    if (landscapePlugin) {
        loadOrder.push_back(&*landscapePlugin);
    } else {
        LOG_WARN("landscape.toml failed to load — using built-in defaults");
    }
    if (adventurePlugin) {
        loadOrder.push_back(&*adventurePlugin);
    } else {
        LOG_WARN("adventure.toml failed to load — no props/NPCs");
    }
    data::resolve(loadOrder, formTypes, forms);
    for (const data::Plugin* plugin : loadOrder) {
        for (const data::AssetEntry& entry : plugin->assets) {
            assetDb.add(entry.id, plugin->baseDir, entry.path);
        }
    }
    tuning = resolveLandscapeTuning(forms);
    weathers = resolveWeatherForms(forms);
    LOG_INFO("Landscape tuning: seed={} seaLevel={} fogDensity={} "
             "coverage={} | {} weather states",
             tuning.terrainSeed, tuning.seaLevel, tuning.fogDensity,
             tuning.cloudCoverage, weathers.size());

    // Terrain shape + startup values for every live-adjustable knob.
    terrain.params.seed = tuning.terrainSeed;
    terrain.params.hillWavelength = tuning.hillWavelength;
    terrain.params.hillAmplitude = tuning.hillAmplitude;
    terrain.params.mountainWavelength = tuning.mountainWavelength;
    terrain.params.mountainAmplitude = tuning.mountainAmplitude;
    terrain.params.seaLevel = tuning.seaLevel;
    fogDensityUi = tuning.fogDensity;
    fogHeightFalloffUi = tuning.fogHeightFalloff;
    fogLowBoostUi = tuning.fogLowBoost;
    fogStartUi = tuning.fogStart;
    exposureUi = tuning.exposure;
    bloomIntensityUi = tuning.bloomIntensity;
    godRayIntensityUi = tuning.godRayIntensity;
    volumetricUi = tuning.volumetricIntensity;
    ssaoUi = tuning.ssaoStrength;
    cloudCoverageUi = tuning.cloudCoverage;
    cloudShadowUi = tuning.cloudShadowStrength;
    cloudHeightUi = tuning.cloudHeight;
    cloudScaleUi = tuning.cloudScale;

    frameUbo = device.createBuffer({ .usage = rhi::BufferUsage::Uniform,
                                     .size = sizeof(render::FrameUniforms),
                                     .dynamic = true },
                                   nullptr);
    frameBindGroup = device.createBindGroup(
        { .entries = { { .binding = 0, .buffer = frameUbo } } });

    shaders = std::make_unique<render::ShaderLibrary>(device);
    terrain.create(device, *shaders, engine->getJobSystem());
    occlusion.create(engine->getJobSystem());
    grass.create(device, *shaders, engine->getJobSystem());
    vegetation.create(device, *shaders, engine->getJobSystem(),
                      terrain.params.seed);

    // B1: the real mesh path. Plugin ReferenceForms spawn into a small ECS
    // world through the Spawner (§2.7 — MeshRender wired by reflection from
    // the base form's model/material); extractMeshes fills the snapshot;
    // the residency caches resolve guids at draw time (§7).
    materialTextures = std::make_unique<TextureCache>(
        device, assetDb, engine->getJobSystem(),
        TextureCache::UploadDesc { .format = rhi::TextureFormat::SRGBA8,
                                   .filter = rhi::FilterMode::Linear });
    meshCache =
        std::make_unique<MeshCache>(device, assetDb, engine->getJobSystem());
    const u32 white = 0xFFFFFFFF;
    whiteTexture = device.createTexture(
        { .width = 1, .height = 1, .format = rhi::TextureFormat::SRGBA8 },
        &white);
    meshSampler = device.createSampler({});
    shaders->load("mesh", { { "FrameUbo", 0 }, { "ModelUbo", 1 } },
                  { { "uAlbedo", 0 } });
    buildMeshPipeline(device);

    // B4: the sim-side physics world + terrain collision (tiles follow the
    // camera for now; the player becomes the focus in B5).
    physics = std::make_unique<phys::PhysicsWorld>();
    terrainCollision =
        std::make_unique<TerrainCollision>(*physics, terrain.params);

    // B5.5: the character-stats runtime shared by every actor in the scene
    // (the player first; the NPC joins in B6) — same setup as CombatArena.
    statsTuning = gameplay::resolveStatsTuning(forms);
    derivedStats = gameplay::DerivedStatRegistry {};
    gameplay::registerCoreDerivedStats(derivedStats, statsTuning);
    gameTags = gameplay::GameplayTagRegistry {};
    gameTags.registerTag("State.Dead");
    gameTags.registerTag("State.Staggered");
    gameTags.registerTag("State.Paralyzed");
    gameTags.registerTag("State.Exhausted");
    for (const char* statusTag :
         { "Status.Poisoned", "Status.Bleeding", "Status.Mental",
           "Status.Diseased", "Status.Cursed", "Status.Dying",
           "Status.HarmonyBroken", "Status.Ignited", "Status.Glaciated",
           "Status.Electrocuted" }) {
        gameTags.registerTag(statusTag);
    }
    gameplay::registerStatsRuntimeTags(gameTags);
    sprintCostEffect =
        data::findByEditorId<gameplay::EffectForm>(forms, "SprintCost");
    testWoundEffect =
        data::findByEditorId<gameplay::EffectForm>(forms, "TestLegWound");

    world = ecs::World {}; // fresh on re-enter
    world::registerSceneComponents(world);
    gameplay::registerGameplayComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);
    world::SpawnContext spawnCtx { world, forms, categories };
    const auto* playerForm =
        data::findByEditorId<data::ActorForm>(forms, "Player");
    playerEntity = ecs::Entity {};
    u32 spawned = 0;
    data::forEach<world::ReferenceForm>(
        forms, [&](const world::ReferenceForm& reference) {
            if (!reference.enabled || reference.prefab.isValid()) {
                return; // disabled, or a prefab TEMPLATE (never self-spawns)
            }
            const ecs::Entity entity =
                spawner.spawn(spawnCtx, reference, ecs::Entity {});
            if (entity.is_alive()) {
                ++spawned;
                if (playerForm && reference.baseForm == playerForm->id) {
                    playerEntity = entity;
                }
            }
        });
    if (playerEntity.is_alive()) {
        const gameplay::CharacterTickContext tickCtx { derivedStats,
                                                       gameTags, statsTuning };
        gameplay::initializeActorStats(playerEntity, tickCtx);
    } else {
        LOG_WARN("B5.5: no Player actor spawned — controller falls back to "
                 "fixed speeds");
    }
    // B1 convention: authored position.y is an offset above the terrain —
    // ground every mesh prop (hand-authored heights arrive with the level
    // editor, chantier 2).
    world.handle()
        .query<world::Transform, const world::MeshRender>()
        .each([&](flecs::entity, world::Transform& transform,
                  const world::MeshRender&) {
            transform.position.y += render::terrain::height(
                terrain.params, transform.position.x, transform.position.z);
        });
    LOG_INFO("B1: {} references spawned from the plugin stack", spawned);

    // B6: Forms-driven NPCs — every spawned actor whose ActorForm resolves
    // an ActorVisual gets its GPU skin, its data-built locomotion graph,
    // and its patrol brain. The scene builds no character by hand anymore.
    shaders->load("skinned", { { "FrameUbo", 0 }, { "ModelUbo", 1 } },
                  { { "uAlbedo", 0 } });
    buildSkinnedPipeline(device);
    setupNpcs(device);

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
        water.create(device, *shaders, engine->getJobSystem());
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
                        { "uSsao", 4 } });
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

    // Start beside the NPC (slightly above, looking at it) — never
    // inside the terrain: the spot is grounded on the SAME height function
    // the mesh uses. Fallback: safely above the demo area.
    if (!npcs.empty()) {
        flyCamera.camera.position = characterSpot + Vec3 { 2.5f, 2.0f, 7.0f };
        const Vec3 look = glm::normalize(characterSpot +
                                         Vec3 { 0.0f, 0.5f, 0.0f } -
                                         flyCamera.camera.position);
        flyCamera.camera.yaw = std::atan2(look.x, -look.z);
        flyCamera.camera.pitch = std::asin(look.y);
    } else {
        const f32 ground = render::terrain::height(terrain.params, 32.0f,
                                                   400.0f);
        flyCamera.camera.position = { 32.0f, ground + 30.0f, 400.0f };
        flyCamera.camera.pitch = -0.30f;
    }
    // Cover the full streamed ring (~14 chunks = ~900 m) plus headroom.
    flyCamera.camera.farPlane = 1600.0f;
}

void LandscapeScene::onExit() {
    rhi::Device& device = engine->getDevice();
    engine->getWindow().setRelativeMouseMode(false);
    destroyOffscreenTarget(device);
    device.destroyPipeline(blitPipeline);
    device.destroySampler(blitSampler);
    // B1 mesh path: per-entry draw state, then the caches (their dtors free
    // the GPU resources they own — device is alive here).
    for (MeshDraw& draw : meshDraws) {
        if (draw.group.id != 0) {
            device.destroyBindGroup(draw.group);
        }
        if (draw.ubo.id != 0) {
            device.destroyBuffer(draw.ubo);
        }
    }
    meshDraws.clear();
    snapshot = RenderSnapshot {};
    meshCache.reset();
    materialTextures.reset();
    device.destroyPipeline(meshPipeline);
    // B6 NPCs: GPU state per NPC, then the CPU-side rig cache.
    for (auto& npc : npcs) {
        npc->anim.reset(); // references npc->graph — release it first
        device.destroyBindGroup(npc->group);
        device.destroyBuffer(npc->modelUbo);
        device.destroyBuffer(npc->paletteSsbo);
        device.destroyBuffer(npc->indices);
        device.destroyBuffer(npc->vertices);
    }
    npcs.clear();
    patrolPoints.clear();
    rigCache.clear();
    device.destroyPipeline(skinnedPipeline);
    skinnedPipeline = {};
    // B4/B5 physics: bodies -> tiles -> world (each references the previous).
    playMode = false;
    player.reset();
    debugCapsule.reset();
    terrainCollision.reset();
    physics.reset();
    device.destroySampler(meshSampler);
    device.destroyTexture(whiteTexture);
    gpuOcclusion.destroy(device);
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
    device.destroyBuffer(frameUbo);
}

void LandscapeScene::ensureOffscreenTarget(rhi::Device& device, u32 width,
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
    blitBindGroup = device.createBindGroup(
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
                          .sampler = blitSampler } }
                  : vector<rhi::BindGroupEntry> {
                        { .binding = 0,
                          .texture = offscreenColor,
                          .sampler = blitSampler } } });
    offscreenWidth = width;
    offscreenHeight = height;
}

void LandscapeScene::destroyOffscreenTarget(rhi::Device& device) {
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
    device.destroyBindGroup(blitBindGroup);
    device.destroyFramebuffer(offscreenFb);
    device.destroyTexture(offscreenDepth);
    device.destroyTexture(offscreenColor);
    blitBindGroup = {};
    offscreenFb = {};
    offscreenDepth = {};
    offscreenColor = {};
    offscreenWidth = 0;
    offscreenHeight = 0;
}

void LandscapeScene::rebuildBlitPipeline(rhi::Device& device) {
    if (blitPipeline.id != 0) {
        device.destroyPipeline(blitPipeline);
    }
    blitPipeline =
        device.createPipeline({ .shader = shaders->get(kTonemapShader) });
    blitShaderGeneration = shaders->generation(kTonemapShader);
}

void LandscapeScene::update(f32 dt) {
    timeSeconds += dt;
    // B1 mesh path: pump async residency (worker decodes -> main-thread
    // uploads, §7), then extract this frame's snapshot from the world.
    if (materialTextures) {
        materialTextures->pumpUploads();
    }
    if (meshCache) {
        meshCache->pumpUploads();
    }
    // B4/B5: physics tick + collision tiles around the focus (the player
    // in Play mode, the camera in Fly); the debug capsule free-falls.
    if (physics) {
        physics->tick(dt);
        terrainCollision->update(playMode && player
                                     ? player->position()
                                     : flyCamera.camera.position);
        if (debugCapsule) {
            debugCapsule->move({ 0.0f, 0.0f, 0.0f }, dt);
        }
    }
    // B5.5: the character pipeline ticks the player (effects, regen,
    // exhaustion, injuries...) — game time 1:1 with real time until the
    // game clock joins the scene.
    if (playerEntity.is_alive()) {
        const gameplay::CharacterTickContext tickCtx { derivedStats,
                                                       gameTags, statsTuning };
        gameplay::tickCharacter(playerEntity, dt, dt, tickCtx);
    }
    snapshot.meshes.clear();
    extractMeshes(world, snapshot);
    if (debugCapsule) {
        // Visualize as the residency placeholder box (magenta), stretched
        // to the capsule's stance, standing at the FEET position.
        const Mat4 transform =
            glm::scale(glm::translate(Mat4 { 1.0f }, debugCapsule->position()),
                       Vec3 { 0.9f, 2.25f, 0.9f });
        snapshot.meshes.push_back({ core::Guid {}, core::Guid {}, transform });
    }
    // B6: patrol + graph-driven poses for every Forms-built NPC.
    updateNpcs(dt);
    // Wind phase integrates the CURRENT strength: speed changes bend the
    // drift/sway smoothly instead of teleporting the pattern.
    windTime += dt * glm::max(windStrengthUi, 0.05f);

    // Weather crossfade: every parameter slides from the captured start
    // state to the selected weather over weatherDuration seconds.
    if (weatherBlend < 1.0f && weatherSelected >= 0 &&
        weatherSelected < static_cast<i32>(weathers.size())) {
        weatherBlend = glm::min(
            weatherBlend + dt / glm::max(weatherDuration, 0.01f), 1.0f);
        const WeatherForm& to = weathers[weatherSelected];
        const f32 t = glm::smoothstep(0.0f, 1.0f, weatherBlend);
        WeatherForm blended;
        const auto lerp = [t](f32 a, f32 b) { return glm::mix(a, b, t); };
        blended.cloudCoverage = lerp(weatherFrom.cloudCoverage,
                                     to.cloudCoverage);
        blended.cloudScale = lerp(weatherFrom.cloudScale, to.cloudScale);
        blended.cloudHeight = lerp(weatherFrom.cloudHeight, to.cloudHeight);
        blended.cloudShadowStrength = lerp(weatherFrom.cloudShadowStrength,
                                           to.cloudShadowStrength);
        blended.fogDensity = lerp(weatherFrom.fogDensity, to.fogDensity);
        blended.fogHeightFalloff = lerp(weatherFrom.fogHeightFalloff,
                                        to.fogHeightFalloff);
        blended.fogLowBoost = lerp(weatherFrom.fogLowBoost, to.fogLowBoost);
        blended.fogStart = lerp(weatherFrom.fogStart, to.fogStart);
        blended.sunIntensity = lerp(weatherFrom.sunIntensity,
                                    to.sunIntensity);
        blended.ambientIntensity = lerp(weatherFrom.ambientIntensity,
                                        to.ambientIntensity);
        blended.saturation = lerp(weatherFrom.saturation, to.saturation);
        blended.warmth = lerp(weatherFrom.warmth, to.warmth);
        blended.volumetricIntensity = lerp(weatherFrom.volumetricIntensity,
                                           to.volumetricIntensity);
        blended.godRayIntensity = lerp(weatherFrom.godRayIntensity,
                                       to.godRayIntensity);
        blended.bloomIntensity = lerp(weatherFrom.bloomIntensity,
                                      to.bloomIntensity);
        blended.windStrength = lerp(weatherFrom.windStrength,
                                    to.windStrength);
        blended.waveChop = lerp(weatherFrom.waveChop, to.waveChop);
        applyWeather(blended);
    }

    // B5: F toggles first-person Play mode (unless ImGui owns the
    // keyboard). In Play the player drives; Fly stays the dev camera.
    if (engine->getInput().wasPressed(platform::Key::F) &&
        !ImGui::GetIO().WantCaptureKeyboard) {
        playMode ? exitPlayMode() : enterPlayMode();
    }
    if (playMode && player) {
        updatePlayer(dt);
    } else {
        // Don't steal the mouse from ImGui: clicking a panel must not
        // mouselook.
        const bool allowCapture = !ImGui::GetIO().WantCaptureMouse;
        flyCamera.update(engine->getInput(), engine->getWindow(), dt,
                         allowCapture);
    }
    if (animateTime) {
        // Full day/night cycle in two minutes.
        sky.timeOfDay += dt * (24.0f / 120.0f);
        if (sky.timeOfDay >= 24.0f) {
            sky.timeOfDay -= 24.0f;
        }
    }
}

// Draws the snapshot's mesh section in the opaque pass: guids resolve
// through the residency caches (placeholder box / checker while pending);
// one small ModelUbo + bind group per entry, recreated only when the bound
// texture flips (placeholder -> resident) or the material changes. N stays
// tiny in B1; grouping/instancing per (model, material) is the contract's
// planned next step (HORIZONTAL-PASS, monde 3D note). No shadow cast yet —
// parity with the H8 cube; casters join with the interiors chantier.
void LandscapeScene::drawSceneMeshes(engine::FrameContext& frame) {
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
        const MeshCache::Gpu& mesh = meshCache->resolve(instance.model);

        ModelUniforms uniforms;
        uniforms.model = instance.transform;
        rhi::TextureHandle albedo = whiteTexture;
        if (const auto* material =
                forms.find<data::MaterialForm>(instance.material)) {
            uniforms.tint = material->tint;
            uniforms.info.x = material->emissive;
            if (material->albedoTexture.isValid()) {
                const rhi::TextureHandle resolved =
                    materialTextures->resolve(material->albedoTexture);
                if (resolved.id != 0) {
                    albedo = resolved;
                }
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

void LandscapeScene::enterPlayMode() {
    if (!physics) {
        return;
    }
    // Spawn the capsule under the camera, feet grounded on the height
    // function (+0.5 m so a slope never pins the spawn into the field).
    Vec3 feet = flyCamera.camera.position;
    feet.y = render::terrain::height(terrain.params, feet.x, feet.z) + 0.5f;
    player =
        std::make_unique<phys::CharacterBody>(*physics, 0.3f, 1.8f, feet);
    playerVelocity = Vec3 { 0.0f };
    playMode = true;
    engine->getWindow().setRelativeMouseMode(true);
}

void LandscapeScene::exitPlayMode() {
    playMode = false;
    player.reset();
    engine->getWindow().setRelativeMouseMode(false);
    // The camera stays where the player stood — Fly resumes from there.
}

void LandscapeScene::updatePlayer(f32 dt) {
    platform::Input& input = engine->getInput();

    // Mouselook, always captured in Play (no LMB gymnastics in a game).
    const Vec2 look = input.mouseDelta();
    flyCamera.camera.yaw += look.x * flyCamera.lookSensitivity;
    flyCamera.camera.pitch = glm::clamp(
        flyCamera.camera.pitch - look.y * flyCamera.lookSensitivity,
        glm::radians(-89.0f), glm::radians(89.0f));

    // Camera-relative intent, flattened to the horizontal plane (§ the
    // controller OWNS motion — anims stay in place).
    const f32 yaw = flyCamera.camera.yaw;
    const Vec3 forward { std::sin(yaw), 0.0f, -std::cos(yaw) };
    const Vec3 right { std::cos(yaw), 0.0f, std::sin(yaw) };
    Vec3 wish { 0.0f };
    if (input.isDown(platform::Key::W)) {
        wish += forward;
    }
    if (input.isDown(platform::Key::S)) {
        wish -= forward;
    }
    if (input.isDown(platform::Key::D)) {
        wish += right;
    }
    if (input.isDown(platform::Key::A)) {
        wish -= right;
    }
    const bool moving = glm::dot(wish, wish) > 0.0f;

    // B5.5: speeds come from the CURRENT derived stats (docs/STATS.md §3
    // — stat-space ~100 = nominal; injuries/buffs move them live). The
    // controller only READS attributes (§2.9); sprint pays energy through
    // the SprintCost effect below. Fallback keeps the scene alive without
    // a Player actor.
    f32 jog = 100.0f * kSpeedScale3D;
    f32 accelRate = 100.0f * kAccelRate3D;
    f32 energy = 100.0f;
    if (playerEntity.is_alive()) {
        const auto& sys = playerEntity.get<gameplay::AbilitySystem>();
        jog = gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
              kSpeedScale3D;
        accelRate =
            gameplay::currentValueOf(sys, gameplay::attr("acceleration")) *
            kAccelRate3D;
        energy = gameplay::currentValueOf(sys, gameplay::attr("energy"));
    }
    const bool sprinting = moving && input.isDown(platform::Key::Shift) &&
                           energy > 1.0f;
    const f32 targetSpeed = sprinting ? jog * kSprintMult : jog;
    const Vec3 target =
        moving ? glm::normalize(wish) * targetSpeed : Vec3 { 0.0f };
    // Exponential smoothing toward the target: snappy, never binary.
    playerVelocity += (target - playerVelocity) *
                      (1.0f - std::exp(-accelRate * dt));
    if (input.wasPressed(platform::Key::Space)) {
        player->jump(jumpSpeed);
    }
    player->move(playerVelocity, dt);

    // Sprint cost: one instant GameplayEffect per half second (§2.9 — the
    // ONLY way energy moves; the spend also pauses regen for a beat).
    if (sprinting && sprintCostEffect && playerEntity.is_alive()) {
        sprintCostAccumulator += dt;
        while (sprintCostAccumulator >= 0.5f) {
            sprintCostAccumulator -= 0.5f;
            auto& set = playerEntity.get_mut<gameplay::AttributeSet>();
            auto& sys = playerEntity.get_mut<gameplay::AbilitySystem>();
            gameplay::applyEffect(set, sys, *sprintCostEffect, gameTags);
        }
    } else {
        sprintCostAccumulator = 0.0f;
    }

    // Eyes 1.70 m above the feet; the ENTITY transform tracks the capsule
    // (the sim's view of the player — extract/saves read this, not Jolt).
    flyCamera.camera.position =
        player->position() + Vec3 { 0.0f, 1.7f, 0.0f };
    if (playerEntity.is_alive()) {
        playerEntity.get_mut<world::Transform>().position =
            player->position();
    }
}

// --- B6: Forms-driven NPCs --------------------------------------------------------

namespace {
constexpr f32 kNpcPauseSeconds = 2.5f; // idle beat at each patrol end
constexpr f32 kNpcWalkFactor = 0.35f;  // of the jog speed (STATS.md: walk
                                       // divides) — a stroll, not a march
} // namespace

const LandscapeScene::RigData* LandscapeScene::loadRig(
    const core::Guid& asset) {
    if (const auto it = rigCache.find(asset); it != rigCache.end()) {
        return it->second.skeleton.joints.empty() ? nullptr : &it->second;
    }
    RigData& rig = rigCache[asset]; // empty entry = negative cache
    const auto path = assetDb.resolve(asset);
    if (!path) {
        LOG_WARN("B6: no asset registered for rig {}", asset.toString());
        return nullptr;
    }
    auto skeleton = assets::loadGltfSkeleton(*path);
    if (!skeleton) {
        return nullptr;
    }
    rig.skeleton = std::move(*skeleton);
    rig.clips = assets::loadGltfAnimations(*path, rig.skeleton);
    LOG_INFO("B6: rig {} loaded — {} joints, {} clips", path->string(),
             rig.skeleton.joints.size(), rig.clips.size());
    return &rig;
}

void LandscapeScene::setupNpcs(rhi::Device& device) {
    // Patrol points: every "patrol" marker, grounded, in spawn order.
    patrolPoints.clear();
    world.handle()
        .query<world::Transform, const world::MarkerKind>()
        .each([&](flecs::entity, world::Transform& transform,
                  const world::MarkerKind& marker) {
            if (marker.kind == "patrol") {
                transform.position.y = render::terrain::height(
                    terrain.params, transform.position.x,
                    transform.position.z);
                patrolPoints.push_back(transform.position);
            }
        });

    const gameplay::CharacterTickContext tickCtx { derivedStats, gameTags,
                                                   statsTuning };
    world.handle()
        .query<world::Transform, const world::RefId>()
        .each([&](flecs::entity e, world::Transform& transform,
                  const world::RefId& ref) {
            ecs::Entity entity { e };
            if (!entity.has<world::ActorMarker>() ||
                entity == playerEntity) {
                return;
            }
            const data::Form* base = forms.get(ref.base);
            const reflect::TypeInfo* type = forms.typeOf(ref.base);
            if (!base || !type ||
                !type->isA(data::ActorForm::staticTypeInfo().id)) {
                return;
            }
            const auto& actor = *static_cast<const data::ActorForm*>(base);
            const auto visual = world::resolveActorVisual(forms, actor);
            if (!visual) {
                return; // 2D/legacy actor (the Player has no appearance)
            }
            const RigData* rig = loadRig(visual->skeleton);
            if (!rig) {
                return;
            }
            const auto meshPath = assetDb.resolve(visual->mesh);
            auto skinned =
                meshPath ? assets::loadGltfSkinnedMesh(*meshPath)
                         : std::nullopt;
            if (!skinned) {
                return;
            }
            auto graph = world::buildAnimGraph(
                forms, visual->animGraph,
                [&](const core::Guid& asset,
                    const str& name) -> std::optional<anim::AnimClip> {
                    const RigData* clipRig = loadRig(asset);
                    if (!clipRig) {
                        return std::nullopt;
                    }
                    for (const assets::GltfClip& clip : clipRig->clips) {
                        if (name.empty() || clip.name == name) {
                            return clip.clip;
                        }
                    }
                    LOG_WARN("B6: no animation '{}' in rig {}", name,
                             asset.toString());
                    return std::nullopt;
                });
            if (!graph) {
                return;
            }

            auto npc = std::make_unique<Npc>();
            npc->entity = entity;
            npc->rig = rig;
            npc->graph = std::move(*graph);
            npc->anim = std::make_unique<anim::GraphInstance>(npc->graph);
            // No event sink yet: timeline events (Footstep...) get their
            // consumers (cues, audio-by-material) in the « vivant »
            // chantier — logging each step only floods the console.
            npc->tint = visual->tint;
            npc->palette.assign(rig->skeleton.joints.size(), Mat4 { 1.0f });
            npc->vertices = device.createBuffer(
                { .usage = rhi::BufferUsage::Vertex,
                  .size = skinned->vertices.size() *
                          sizeof(render::SkinnedVertex) },
                skinned->vertices.data());
            npc->indices = device.createBuffer(
                { .usage = rhi::BufferUsage::Index,
                  .size = skinned->indices.size() * sizeof(u32) },
                skinned->indices.data());
            npc->indexCount = static_cast<u32>(skinned->indices.size());
            npc->paletteSsbo = device.createBuffer(
                { .usage = rhi::BufferUsage::Storage,
                  .size = npc->palette.size() * sizeof(Mat4),
                  .dynamic = true },
                npc->palette.data());
            npc->modelUbo = device.createBuffer(
                { .usage = rhi::BufferUsage::Uniform,
                  // std140 ModelUbo: mat4 model + vec4 tint + vec4 info.
                  .size = sizeof(Mat4) + 2 * sizeof(Vec4),
                  .dynamic = true },
                nullptr);
            npc->group = device.createBindGroup(
                { .entries = { { .binding = 1, .buffer = npc->modelUbo },
                               { .binding = 0,
                                 .texture = whiteTexture,
                                 .sampler = meshSampler },
                               { .binding = 2,
                                 .buffer = npc->paletteSsbo,
                                 .storage = true } } });

            // Ground the entity (actors have no MeshRender: the B1 snap
            // skipped them) and give it a full stat sheet.
            transform.position.y = render::terrain::height(
                terrain.params, transform.position.x, transform.position.z);
            gameplay::initializeActorStats(entity, tickCtx);

            if (npcs.empty()) {
                characterSpot = transform.position;
            }
            npcs.push_back(std::move(npc));
        });
    LOG_INFO("B6: {} NPC(s) built from Forms, {} patrol point(s)",
             npcs.size(), patrolPoints.size());
}

void LandscapeScene::updateNpcs(f32 dt) {
    for (auto& npcPtr : npcs) {
        Npc& npc = *npcPtr;
        auto& transform = npc.entity.get_mut<world::Transform>();

        // The stroll speed comes from the NPC's OWN stats (§C.1: the actor
        // is the character definition — a wounded villager limps).
        const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
        const f32 walkSpeed =
            gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
            kSpeedScale3D * kNpcWalkFactor;

        f32 targetSpeed = 0.0f;
        if (patrolPoints.size() >= 2) {
            const Vec3 goal = patrolPoints[npc.target % patrolPoints.size()];
            Vec3 to = goal - transform.position;
            to.y = 0.0f;
            const f32 distance = glm::length(to);
            if (npc.pauseTimer > 0.0f) {
                npc.pauseTimer -= dt; // idle beat at the end point
            } else if (distance < 0.4f) {
                npc.pauseTimer = kNpcPauseSeconds;
                npc.target = (npc.target + 1) %
                             static_cast<u32>(patrolPoints.size());
            } else {
                const Vec3 dir = to / distance;
                targetSpeed = walkSpeed;
                transform.position += dir * walkSpeed * dt;
                transform.position.y = render::terrain::height(
                    terrain.params, transform.position.x,
                    transform.position.z);
                // Face the walk direction (mannequin authored facing +Z),
                // smoothed over ~0.15 s.
                const f32 goalYaw = std::atan2(dir.x, dir.z);
                f32 delta = goalYaw - npc.yaw;
                while (delta > glm::pi<f32>()) {
                    delta -= glm::two_pi<f32>();
                }
                while (delta < -glm::pi<f32>()) {
                    delta += glm::two_pi<f32>();
                }
                npc.yaw += delta * (1.0f - std::exp(-8.0f * dt));
                transform.rotation =
                    glm::angleAxis(npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
            }
        }
        npc.speed += (targetSpeed - npc.speed) *
                     (1.0f - std::exp(-10.0f * dt));

        // Anim: real speed feeds the param (transitions) AND the
        // referenceSpeed sync (anti-foot-sliding).
        npc.anim->setParam("speed", npc.speed);
        npc.anim->update(dt, npc.speed);
        anim::bindPose(npc.rig->skeleton, npc.pose);
        npc.anim->evaluate(npc.pose);
        anim::skinMatrices(npc.rig->skeleton, npc.pose, npc.palette);
    }
}

void LandscapeScene::drawNpcs(engine::FrameContext& frame) {
    if (npcs.empty()) {
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
    frame.cmd.setPipeline(skinnedPipeline);
    frame.cmd.setBindGroup(0, frameBindGroup);
    for (auto& npcPtr : npcs) {
        Npc& npc = *npcPtr;
        const auto& transform = npc.entity.get<world::Transform>();
        ModelUniforms uniforms;
        uniforms.model =
            glm::translate(Mat4 { 1.0f }, transform.position) *
            glm::mat4_cast(transform.rotation);
        uniforms.tint = npc.tint;
        frame.device.updateBuffer(npc.modelUbo, &uniforms, sizeof(uniforms),
                                  0);
        frame.device.updateBuffer(npc.paletteSsbo, npc.palette.data(),
                                  npc.palette.size() * sizeof(Mat4), 0);
        frame.cmd.setBindGroup(1, npc.group);
        frame.cmd.setVertexBuffer(0, npc.vertices);
        frame.cmd.setIndexBuffer(npc.indices, rhi::IndexFormat::U32);
        frame.cmd.drawIndexed(npc.indexCount);
    }
}

void LandscapeScene::buildSkinnedPipeline(rhi::Device& device) {
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

void LandscapeScene::buildMeshPipeline(rhi::Device& device) {
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

WeatherForm LandscapeScene::captureCurrentWeather() const {
    WeatherForm w;
    w.cloudCoverage = cloudCoverageUi;
    w.cloudScale = cloudScaleUi;
    w.cloudHeight = cloudHeightUi;
    w.cloudShadowStrength = cloudShadowUi;
    w.fogDensity = fogDensityUi;
    w.fogHeightFalloff = fogHeightFalloffUi;
    w.fogLowBoost = fogLowBoostUi;
    w.fogStart = fogStartUi;
    w.sunIntensity = sunIntensityUi;
    w.ambientIntensity = ambientIntensityUi;
    w.saturation = saturationUi;
    w.warmth = warmthUi;
    w.volumetricIntensity = volumetricUi;
    w.godRayIntensity = godRayIntensityUi;
    w.bloomIntensity = bloomIntensityUi;
    w.windStrength = windStrengthUi;
    w.waveChop = waveChopUi;
    return w;
}

void LandscapeScene::applyWeather(const WeatherForm& w) {
    cloudCoverageUi = w.cloudCoverage;
    cloudScaleUi = w.cloudScale;
    cloudHeightUi = w.cloudHeight;
    cloudShadowUi = w.cloudShadowStrength;
    fogDensityUi = w.fogDensity;
    fogHeightFalloffUi = w.fogHeightFalloff;
    fogLowBoostUi = w.fogLowBoost;
    fogStartUi = w.fogStart;
    sunIntensityUi = w.sunIntensity;
    ambientIntensityUi = w.ambientIntensity;
    saturationUi = w.saturation;
    warmthUi = w.warmth;
    volumetricUi = w.volumetricIntensity;
    godRayIntensityUi = w.godRayIntensity;
    bloomIntensityUi = w.bloomIntensity;
    windStrengthUi = w.windStrength;
    waveChopUi = w.waveChop;
}

void LandscapeScene::render(engine::FrameContext& frame) {
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
    terrain.update(frame.device, flyCamera.camera.position);
    // Height-horizon occlusion (brick 26): rebuilt on a worker whenever
    // the camera strays; the set stays valid (conservative) meanwhile.
    occlusion.pump();
    if (occlusion.wantsRebuild(flyCamera.camera.position)) {
        occlusion.rebuild(terrain.params, flyCamera.camera.position,
                          terrain.chunkTops());
    }
    grass.update(frame.device, terrain.params, flyCamera.camera.position);
    vegetation.update(frame.device, terrain.params,
                      flyCamera.camera.position);
    if (frame.device.caps().copyTexture) {
        water.update(frame.device, terrain.params, flyCamera.camera.position);
    }

    const render::Camera3D& camera = flyCamera.camera;
    const Mat4 viewProj = camera.viewProj(frame.aspect);
    // CPU chunk culling (brick 25): one frustum per rendered viewpoint.
    const render::Frustum viewFrustum = render::Frustum::fromViewProj(viewProj);
    const render::SkySystem::SkyState skyState =
        sky.evaluate({ .cloudCoverage = cloudCoverageUi,
                       .sunIntensity = sunIntensityUi,
                       .ambientIntensity = ambientIntensityUi,
                       .saturation = saturationUi,
                       .warmth = warmthUi });

    // Shadows ramp out as the sun crosses the horizon (no sun, no shadows),
    // and soften away under heavy cloud cover (diffuse light casts none).
    const bool shadowsAvailable = shadows.receiverBindGroup().id != 0;
    const f32 shadowStrength =
        (shadowsUi && shadowsAvailable)
            ? glm::smoothstep(-0.02f, 0.06f, skyState.sunDirection.y) *
                  (1.0f - 0.65f * cloudCoverageUi)
            : 0.0f;
    render::ShadowMapper::Cascades cascades {};
    if (shadowStrength > 0.0f) {
        cascades = shadows.computeCascades(camera, frame.aspect,
                                           skyState.sunDirection);
        shadows.updateCascadeUbos(frame.device, cascades);
    }

    // Planar reflection is meaningful only from above the surface.
    const bool reflectionsActive =
        reflectionsUi && reflectionFb.id != 0 &&
        camera.position.y > terrain.params.seaLevel;

    // Sun position on screen for the god rays; shafts fade as the sun
    // leaves the frame or dips below the horizon.
    Vec2 sunUv { 0.5f, 0.5f };
    f32 shaftFade = 0.0f;
    {
        const Vec4 clip =
            viewProj *
            Vec4 { camera.position + skyState.sunDirection * 1000.0f, 1.0f };
        if (clip.w > 0.0f) {
            const Vec2 ndc { clip.x / clip.w, clip.y / clip.w };
            sunUv = ndc * 0.5f + Vec2 { 0.5f };
            const f32 edge = glm::max(std::abs(ndc.x), std::abs(ndc.y));
            shaftFade =
                (1.0f - glm::smoothstep(0.85f, 1.35f, edge)) *
                glm::smoothstep(-0.02f, 0.05f, skyState.sunDirection.y);
        }
    }

    const render::FrameUniforms uniforms {
        .viewProj = viewProj,
        .invViewProj = glm::inverse(viewProj),
        .cameraPos = { camera.position, 1.0f },
        .time = { timeSeconds, ssaoUi, volumetricUi,
                  static_cast<f32>(debugBufferUi) },
        .sunDirection = { skyState.sunDirection, 0.0f },
        .sunColor = { skyState.sunColor, skyState.sunDiscIntensity },
        .sunGlowColor = { skyState.glowColor, 0.0f },
        .ambientColor = { skyState.ambientColor,
                          stylizedUi ? 1.0f : 0.0f },
        .zenithColor = { skyState.zenithColor, 0.0f },
        .horizonColor = { skyState.horizonColor, 0.0f },
        .horizonFarColor = { skyState.horizonFarColor, 0.0f },
        .terrainInfo = { terrain.params.seaLevel, tuning.snowLine,
                         tuning.splatUvScale,
                         reflectionsActive ? 1.0f : 0.0f },
        .postInfo = { tonemapUi ? 1.0f : 0.0f, exposureUi,
                      cascadeDebugUi ? 1.0f : 0.0f, bloomIntensityUi },
        .fogInfo = { fogDensityUi, fogHeightFalloffUi, fogLowBoostUi,
                     fogStartUi },
        .sunViewProj = cascades.viewProj,
        .cascadeSplits = { cascades.splitFar[0], cascades.splitFar[1],
                           cascades.splitFar[2], 0.0f },
        .shadowInfo = { cascades.texelWorld[0], cascades.texelWorld[1],
                        cascades.texelWorld[2], shadowStrength },
        .screenInfo = { static_cast<f32>(frame.width),
                        static_cast<f32>(frame.height),
                        1.0f / static_cast<f32>(frame.width),
                        1.0f / static_cast<f32>(frame.height) },
        .cloudInfo = { cloudCoverageUi, cloudHeightUi, cloudScaleUi,
                       cloudShadowUi },
        .sunScreen = { sunUv.x, sunUv.y, shaftFade, godRayIntensityUi },
        .cloudMapInfo = { std::floor(camera.position.x /
                                     (render::SkySystem::kCloudMapSpan /
                                      render::SkySystem::kCloudMapSize)) *
                              (render::SkySystem::kCloudMapSpan /
                               render::SkySystem::kCloudMapSize),
                          std::floor(camera.position.z /
                                     (render::SkySystem::kCloudMapSpan /
                                      render::SkySystem::kCloudMapSize)) *
                              (render::SkySystem::kCloudMapSpan /
                               render::SkySystem::kCloudMapSize),
                          1.0f / render::SkySystem::kCloudMapSpan, 0.0f },
        .waterMapInfo = water.poolMapInfo(),
        .windInfo = { windTime, windStrengthUi, waveChopUi, 0.0f },
    };
    frame.device.updateBuffer(frameUbo, &uniforms, sizeof(uniforms), 0);

    // Bake this frame's cloud field before anything lights with it.
    sky.bakeCloudMap(frame.cmd, frameBindGroup);

    // Cascade passes: depth-only casters from the sun's point of view.
    if (shadowStrength > 0.0f) {
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

    // The sky covers every background pixel — no color clear needed.
    frame.cmd.beginRenderPass(
        { .framebuffer = useOffscreen ? offscreenFb : rhi::FramebufferHandle {},
          .loadOp = rhi::LoadOp::DontCare,
          .depthLoadOp = rhi::LoadOp::Clear });
    if (sky.cloudMapBindGroup().id != 0) {
            frame.cmd.setBindGroup(3, sky.cloudMapBindGroup());
        }
    // Occlusion applies to the main view only: both sets were built for the
    // real camera, not the mirrored one (the grass ring is too close to
    // ever be ridge-occluded — frustum only). CPU horizon ∪ GPU Hi-Z.
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
    terrain.draw(frame.cmd, frameBindGroup, shadows.receiverBindGroup(),
                 &viewFrustum, occludedSet);
    vegetation.draw(frame.cmd, frameBindGroup, shadows.receiverBindGroup(),
                    render::VegetationSystem::kVariantCount, camera.position,
                    /*forceLowDetail=*/false, &viewFrustum, occludedSet);
    grass.draw(frame.cmd, frameBindGroup, shadows.receiverBindGroup(),
               &viewFrustum);
    drawSceneMeshes(frame); // B1: the RenderSnapshot.meshes consumer
    drawNpcs(frame);        // B6: the Forms-driven skinned NPCs
    sky.draw(frame.cmd, frameBindGroup); // after opaque: background only
    frame.cmd.endRenderPass();

    // Water: snapshot the opaque scene (sampling a bound attachment is UB),
    // then compose refraction/reflection/foam back into the HDR target.
    if (useOffscreen && frame.device.caps().copyTexture &&
        waterSceneBindGroup.id != 0) {
        frame.cmd.copyTexture(offscreenColor, sceneColorCopy);
        frame.cmd.copyTexture(offscreenDepth, sceneDepthCopy);

        // GPU Hi-Z occlusion (brick 26): pyramid from this frame's depth
        // snapshot + cull dispatch; the verdict is read back NEXT frame.
        if (frame.device.caps().computeShaders) {
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

        frame.cmd.beginRenderPass({ .framebuffer = offscreenFb,
                                    .loadOp = rhi::LoadOp::Load,
                                    .depthLoadOp = rhi::LoadOp::Load });
        water.draw(frame.cmd, frameBindGroup, waterSceneBindGroup);
        frame.cmd.endRenderPass();
    }

    // Bloom pyramid + god rays + volumetric shafts, composed by the tonemap.
    // Unit 2 (cloud map) persists across the post passes for the march.
    if (useOffscreen) {
        if (sky.cloudMapBindGroup().id != 0) {
            frame.cmd.setBindGroup(3, sky.cloudMapBindGroup());
        }
        postFx.render(frame.cmd, frameBindGroup,
                      shadows.receiverBindGroup());
    }

    if (useOffscreen) {
        // Tonemap composite: HDR scene -> filmic curve -> gamma -> backbuffer.
        frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::DontCare,
                                    .depthLoadOp = rhi::LoadOp::DontCare });
        frame.cmd.setPipeline(blitPipeline);
        frame.cmd.setBindGroup(0, frameBindGroup); // FrameUbo (uPostInfo)
        frame.cmd.setBindGroup(1, blitBindGroup);  // scene color + sampler
        frame.cmd.draw(3);
        frame.cmd.endRenderPass();
    }
}

void LandscapeScene::drawUi() {
    // F10 hides/shows the whole panel (works even while the mouse is
    // captured in Play — ImGui keeps its own keyboard state).
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) {
        uiPanelVisible = !uiPanelVisible;
    }
    if (!uiPanelVisible) {
        return;
    }
    // A themed section: header click and F-key both toggle the same state.
    const auto section = [](const char* label, ImGuiKey key, bool& open) {
        if (ImGui::IsKeyPressed(key, false)) {
            open = !open;
        }
        ImGui::SetNextItemOpen(open, ImGuiCond_Always);
        open = ImGui::CollapsingHeader(label);
        return open;
    };

    ImGui::Begin("Landscape", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate,
                1000.0f / ImGui::GetIO().Framerate);
    const Vec3 p = flyCamera.camera.position;
    ImGui::Text("Position: %.1f  %.1f  %.1f", p.x, p.y, p.z);
    if (playMode) {
        ImGui::TextUnformatted(
            "PLAY  WASD: move | Shift: sprint | Space: jump | F: fly");
    } else {
        ImGui::TextUnformatted(
            "FLY  LMB: look | WASD+E/Q: move | Shift: boost | F: play");
        ImGui::SliderFloat("Fly speed (m/s)", &flyCamera.moveSpeed, 2.0f,
                           150.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    }
    ImGui::TextDisabled("F1-F4: sections | F10: hide panel");
    ImGui::Separator();

    if (section("Gameplay — player, NPC, physics  [F1]", ImGuiKey_F1,
                uiGameplayOpen)) {
        drawGameplayUi();
    }

    if (section("Terrain & streaming  [F2]", ImGuiKey_F2, uiTerrainOpen)) {
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

    if (section("Sky, weather & time  [F3]", ImGuiKey_F3, uiSkyOpen)) {
        drawSkyUi();
    }

    if (section("Rendering & post-FX  [F4]", ImGuiKey_F4, uiRenderOpen)) {
        drawRenderUi();
    }
    ImGui::End();
}

void LandscapeScene::drawSkyUi() {
    ImGui::SliderFloat("Time of day (h)", &sky.timeOfDay, 0.0f, 24.0f,
                       "%.1f");
    ImGui::Checkbox("Animate (24 h in 2 min)", &animateTime);
    if (!weathers.empty()) {
        // "(manual)" entry + one per WeatherForm, separated by '\0' as
        // ImGui::Combo expects (c_str() supplies the double terminator).
        str items = "(manual)";
        items.push_back('\0');
        for (const WeatherForm& w : weathers) {
            items += w.editorId;
            items.push_back('\0');
        }
        int selected = weatherSelected + 1;
        if (ImGui::Combo("Weather", &selected, items.c_str())) {
            weatherSelected = selected - 1;
            if (weatherSelected >= 0) {
                // Depart from whatever is on screen right now — mid-fade
                // switches stay continuous.
                weatherFrom = captureCurrentWeather();
                weatherBlend = 0.0f;
            }
        }
        ImGui::SliderFloat("Transition (s)", &weatherDuration, 1.0f, 120.0f,
                           "%.0f", ImGuiSliderFlags_Logarithmic);
        if (weatherBlend < 1.0f && weatherSelected >= 0) {
            ImGui::SameLine();
            ImGui::Text("%.0f%%", weatherBlend * 100.0f);
        }
        ImGui::SliderFloat("Wind strength", &windStrengthUi, 0.0f, 2.5f,
                           "%.2f");
        ImGui::SliderFloat("Wave chop", &waveChopUi, 0.0f, 2.5f, "%.2f");
        ImGui::SliderFloat("Sun intensity", &sunIntensityUi, 0.0f, 1.5f,
                           "%.2f");
        ImGui::SliderFloat("Ambient intensity", &ambientIntensityUi, 0.0f,
                           1.5f, "%.2f");
        ImGui::SliderFloat("Saturation", &saturationUi, 0.0f, 1.3f, "%.2f");
        ImGui::SliderFloat("Warmth (dawn/dusk)", &warmthUi, 0.0f, 1.0f,
                           "%.2f");
    }
}

void LandscapeScene::drawGameplayUi() {
    if (!npcs.empty()) {
        // B6: the Forms-driven NPC — patrol state + locomotion graph live.
        static constexpr const char* kStateNames[] = { "idle", "walk",
                                                       "run" };
        const Npc& npc = *npcs.front();
        const u32 state = npc.anim->currentState();
        ImGui::Text("NPC: %s%s | %.1f m/s | pause %.1f s",
                    state < 3 ? kStateNames[state] : "?",
                    npc.anim->blending() ? " (blending)" : "", npc.speed,
                    glm::max(npc.pauseTimer, 0.0f));
        if (ImGui::Button("Teleport to NPC")) {
            // Stand 6 m south, eyes 2 m up, looking at the NPC's CURRENT
            // position (it walks).
            const Vec3 spot = npc.entity.get<world::Transform>().position;
            flyCamera.camera.position = spot + Vec3 { 0.0f, 2.0f, 6.0f };
            const Vec3 dir =
                glm::normalize(spot + Vec3 { 0.0f, 1.0f, 0.0f } -
                               flyCamera.camera.position);
            flyCamera.camera.yaw = std::atan2(dir.x, -dir.z);
            flyCamera.camera.pitch = std::asin(dir.y);
        }
        ImGui::Separator();
    }
    if (physics) {
        // B5: first-person Play mode.
        bool play = playMode;
        if (ImGui::Checkbox("Play mode (B5) — press F", &play)) {
            play ? enterPlayMode() : exitPlayMode();
        }
        if (playMode) {
            ImGui::TextUnformatted(
                "WASD: move | Shift: sprint | Space: jump | F: back to Fly");
        }
        if (playerEntity.is_alive()) {
            // B5.5: the stats that DRIVE the controller, live.
            const auto& sys = playerEntity.get<gameplay::AbilitySystem>();
            ImGui::Text(
                "Player: energy %.0f/%.0f | moveSpeed %.0f | accel %.0f "
                "| speed %.1f m/s",
                gameplay::currentValueOf(sys, gameplay::attr("energy")),
                gameplay::currentValueOf(sys, gameplay::attr("maxEnergy")),
                gameplay::currentValueOf(sys,
                                         gameplay::attr("movementSpeed")),
                gameplay::currentValueOf(sys, gameplay::attr("acceleration")),
                glm::length(playerVelocity));
            if (testWoundEffect &&
                ImGui::Button("Test: leg wound, 10 s (B5.5)")) {
                // Click in Fly, press F, walk: half speed until it expires.
                auto& set = playerEntity.get_mut<gameplay::AttributeSet>();
                auto& asys = playerEntity.get_mut<gameplay::AbilitySystem>();
                gameplay::applyEffect(set, asys, *testWoundEffect, gameTags);
            }
        }
        // B4: drop a kinematic capsule from the camera — it falls, lands
        // on the height-field tiles, and rides slopes (magenta box).
        if (ImGui::Button("Drop capsule here (B4)")) {
            debugCapsule = std::make_unique<phys::CharacterBody>(
                *physics, 0.3f, 1.8f, flyCamera.camera.position);
        }
        if (debugCapsule) {
            ImGui::SameLine();
            const Vec3 feet = debugCapsule->position();
            ImGui::Text("%.1f %.1f %.1f %s", feet.x, feet.y, feet.z,
                        debugCapsule->onGround() ? "(grounded)"
                                                 : "(falling)");
        }
        ImGui::Text("Collision tiles: %u", terrainCollision->tileCount());
    }
}

void LandscapeScene::drawRenderUi() {
    ImGui::Checkbox("Stylized lighting (BotW A/B)", &stylizedUi);
    ImGui::Checkbox("Filmic tonemap (A/B)", &tonemapUi);
    ImGui::SliderFloat("Bloom intensity", &bloomIntensityUi, 0.0f, 1.5f,
                       "%.2f");
    ImGui::SliderFloat("God rays intensity", &godRayIntensityUi, 0.0f, 2.0f,
                       "%.2f");
    ImGui::SliderFloat("Volumetric shafts", &volumetricUi, 0.0f, 3.0f,
                       "%.2f");
    ImGui::SliderFloat("SSAO strength", &ssaoUi, 0.0f, 1.0f, "%.2f");
    ImGui::Combo("Debug buffer", &debugBufferUi,
                 "Off\0Bloom\0God rays\0Volumetric\0SSAO\0");
    ImGui::Checkbox("Shadows", &shadowsUi);
    ImGui::SameLine();
    ImGui::Checkbox("Cascade debug tint", &cascadeDebugUi);
    ImGui::Checkbox("Water reflections", &reflectionsUi);
    ImGui::SliderFloat("Exposure", &exposureUi, 0.25f, 3.0f, "%.2f");
    ImGui::SliderFloat("Fog density", &fogDensityUi, 0.0f, 0.004f, "%.4f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Fog height falloff", &fogHeightFalloffUi, 0.001f,
                       0.08f, "%.3f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Fog low-altitude boost", &fogLowBoostUi, 0.0f, 5.0f,
                       "%.1f");
    ImGui::SliderFloat("Fog start (m)", &fogStartUi, 0.0f, 500.0f, "%.0f");
    ImGui::SliderFloat("Cloud coverage", &cloudCoverageUi, 0.0f, 1.0f,
                       "%.2f");
    ImGui::SliderFloat("Cloud shadow strength", &cloudShadowUi, 0.0f, 1.0f,
                       "%.2f");
}

} // namespace game
