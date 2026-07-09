#include "game/scenes/LandscapeScene.hpp"

#include <cmath>
#include <ctime>
#include <filesystem>
#include <sstream>

#include <glm/glm.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/VisualForms.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "game/AllForms.hpp"
#include "game/Barter.hpp"
#include "game/ui/ConsolePanel.hpp"
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
#include "data/forms/UiForms.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/rhi/Device.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/interaction/FurnitureForms.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/Rest.hpp"
#include "world/scene/AnimBridge.hpp"
#include "world/scene/Spawner.hpp"
#include "world/terrain/TerrainPatches.hpp"

namespace game {

namespace {

constexpr const char* kTonemapShader = "tonemap";

// (B5.5 stat->world movement constants moved to PlayerController, audit U4-1.)

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
    bootstrapData();
    createRenderResources(device);
    setupGameplay();
    setupWorldAndStreaming();
    spawnInitialWorld(device);
}

void LandscapeScene::bootstrapData() {
    // Load the moddable data (§5) through the plugin stack (chantier 4 B1):
    // data/plugins.toml declares the load order, the resolver layers every
    // plugin's fields last-writer-wins. One registration site for all
    // families (AllForms) — UI/quest/dialogue records now resolve here too.
    game::registerAllFormTypes(formTypes);
    const auto dataDir = platform::executableDir() / "data";
    data::PluginConfig pluginConfig;
    if (const auto loaded =
            data::loadPluginConfigFile(dataDir / "plugins.toml")) {
        pluginConfig = *loaded;
    } else {
        LOG_WARN("data/plugins.toml missing — defaulting to data/base/*.toml");
        pluginConfig = data::defaultConfigFromDirectory(dataDir / "base");
        for (auto& entry : pluginConfig.entries) {
            entry.file = "base/" + entry.file;
        }
    }
    pluginStack = data::loadPluginStack(dataDir, pluginConfig, formTypes);
    for (const str& error : pluginStack.errors) {
        LOG_WARN("plugin stack: {}", error);
    }
    forms = data::FormDatabase {};   // fresh on re-enter
    assetDb = assets::AssetDatabase {};
    // Chantier 5 B5: a loading game resolves its save file as the LAST
    // layer — one more plugin, the §5 invariant in action (SaveController
    // owns the queued slot + the loadedFromSave flag).
    std::optional<data::Plugin> savePlugin =
        saveController.beginLoad(formTypes);
    vector<const data::Plugin*> loadOrder = data::pointersOf(pluginStack);
    if (savePlugin) {
        loadOrder.push_back(&*savePlugin);
    }
    data::resolve(loadOrder, formTypes, forms);
    for (const data::Plugin& plugin : pluginStack.plugins) {
        for (const data::AssetEntry& entry : plugin.assets) {
            assetDb.add(entry.id, plugin.baseDir, entry.path);
        }
    }
    LOG_INFO("Plugin stack: {} plugins, {} forms",
             pluginStack.plugins.size(), forms.count());
    tuning = resolveLandscapeTuning(forms);
    weather.init(forms);
    LOG_INFO("Landscape tuning: seed={} seaLevel={} fogDensity={} "
             "coverage={} | {} weather states",
             tuning.terrainSeed, tuning.seaLevel, tuning.fogDensity,
             tuning.cloudCoverage, weather.states().size());

    // B8: the authored-terrain overlay rides inside TerrainParams — every
    // consumer (chunk workers, scatter, collision, snaps) is patched at
    // once. Retire the previous overlay instead of freeing it (workers).
    heightPatches = world::buildHeightPatches(forms, assetDb);
    if (!heightPatches->chunks.empty()) {
        LOG_INFO("B8: {} authored terrain patch(es)",
                 heightPatches->chunks.size());
    }

    // Terrain shape + startup values for every live-adjustable knob.
    terrain.params.seed = tuning.terrainSeed;
    terrain.params.patches = heightPatches;
    terrain.params.hillWavelength = tuning.hillWavelength;
    terrain.params.hillAmplitude = tuning.hillAmplitude;
    terrain.params.mountainWavelength = tuning.mountainWavelength;
    terrain.params.mountainAmplitude = tuning.mountainAmplitude;
    terrain.params.seaLevel = tuning.seaLevel;
    atmos.fogDensity = tuning.fogDensity;
    atmos.fogHeightFalloff = tuning.fogHeightFalloff;
    atmos.fogLowBoost = tuning.fogLowBoost;
    atmos.fogStart = tuning.fogStart;
    exposureUi = tuning.exposure;
    atmos.bloomIntensity = tuning.bloomIntensity;
    atmos.godRayIntensity = tuning.godRayIntensity;
    atmos.volumetric = tuning.volumetricIntensity;
    ssaoUi = tuning.ssaoStrength;
    atmos.cloudCoverage = tuning.cloudCoverage;
    atmos.cloudShadow = tuning.cloudShadowStrength;
    atmos.cloudHeight = tuning.cloudHeight;
    atmos.cloudScale = tuning.cloudScale;
    gradeVibranceUi = tuning.gradeVibrance;   // B3 (toggle stays off)
    gradeSplitToneUi = tuning.gradeSplitTone;
    gradeContrastUi = tuning.gradeContrast;
    autoExposureMinUi = tuning.autoExposureMin; // B4 (toggle stays off)
    autoExposureMaxUi = tuning.autoExposureMax;
}

void LandscapeScene::createRenderResources(rhi::Device& device) {
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
    terrain.create(device, *shaders, engine->getJobSystem());
    occlusion.create(engine->getJobSystem());
    terrainLightMap.create(device, engine->getJobSystem()); // 33b/c
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

    // Chantier 4 B2: the RmlUi game UI (screens from UiScreenForm records,
    // documents through the plugins' ui/ roots).
    createGameUi(device);
}

void LandscapeScene::setupGameplay() {
    // B4: the sim-side physics world + terrain collision (tiles follow the
    // camera for now; the player becomes the focus in B5).
    physics = std::make_unique<phys::PhysicsWorld>();
    terrainCollision = std::make_unique<TerrainCollision>(
        *physics, terrain.params, &engine->getJobSystem());
    vegCollision =
        std::make_unique<VegetationCollision>(*physics, terrain.params);

    // Chantier 3 B2: navigation over the SAME height function as
    // everything else (patches included — the pointer rides in params).
    navigator = std::make_unique<world::TerrainNavigator>(
        [this](f32 x, f32 z) {
            return render::terrain::height(terrain.params, x, z);
        });
    furnitureOccupancy = gameplay::FurnitureOccupancy {};

    // B5.5: the character-stats runtime shared by every actor in the scene
    // (the player first; the NPC joins in B6) — same setup as CombatArena.
    statsTuning = gameplay::resolveStatsTuning(forms);
    derivedStats = gameplay::DerivedStatRegistry {};
    gameplay::registerCoreDerivedStats(derivedStats, statsTuning);
    gameTags = gameplay::GameplayTagRegistry {};
    gameplay::registerCharacterRuntimeTags(gameTags); // audit U5-3
    sprintCostEffect =
        data::findByEditorId<gameplay::EffectForm>(forms, "SprintCost");
    testWoundEffect =
        data::findByEditorId<gameplay::EffectForm>(forms, "TestLegWound");
    // Chantier 3 B6: the melee weapons (data — retune in village.toml).
    playerWeapon =
        data::findByEditorId<data::WeaponForm>(forms, "RustySword");
    banditWeapon =
        data::findByEditorId<data::WeaponForm>(forms, "BanditClub");
    // Chantier 4 B5: the currency + the barter trigger (a dialogue node
    // fires "OpenBarter" — the vendor is whoever we're talking to).
    goldForm = data::findByEditorId<data::MiscItemForm>(forms, "GoldCoin");
    // The eventBus is the scene's central hub (dialogue and combat both
    // publish into it). QuestDirector owns the quest/crime/dialogue LOGIC;
    // the subscriptions stay here — `this` is stable for the eventBus
    // lifetime — and delegate to the director with a fresh context.
    eventBus = gameplay::EventBus {};
    eventBus.subscribe(gameplay::eventKind("OpenBarter"),
                       [this](const gameplay::Event&) {
                           uiRouter.openBarterScreen(
                               makeUiRouterContext(),
                               questDirector.dialoguePartner());
                       });
    questDirector.beginScene(makeQuestContext(),
                             saveController.loadedFromSave());
    eventBus.subscribe(gameplay::eventKind("OnAcceptEasternMenace"),
                       [this](const gameplay::Event&) {
                           questDirector.acceptDemoQuest(makeQuestContext());
                       });
    eventBus.subscribe(gameplay::eventKind("OnDeath"),
                       [this](const gameplay::Event& event) {
                           questDirector.handleQuestEvent(makeQuestContext(),
                                                          event);
                       });
    eventBus.subscribe(gameplay::eventKind("OnReportBandit"),
                       [this](const gameplay::Event& event) {
                           questDirector.handleQuestEvent(makeQuestContext(),
                                                          event);
                       });
    eventBus.subscribe(gameplay::eventKind("OnPayFine"),
                       [this](const gameplay::Event&) {
                           questDirector.payFine(makeQuestContext());
                       });
}

void LandscapeScene::setupWorldAndStreaming() {
    world = ecs::World {}; // fresh on re-enter
    world::registerSceneComponents(world);
    gameplay::registerGameplayComponents(world);
    // Per-frame queries, built once against the fresh world.
    doorQuery = world.handle()
                    .query<const world::Transform, const world::DoorTarget>();
    interactQuery =
        world.handle().query<const world::Transform, const world::RefId>();
    streaming.init(world);
    categories = world::FormCategoryRegistry {};
    world::registerCoreCategories(categories);
    spawner = world::Spawner {};
    world::registerCoreSpawners(spawner);

    // Chantier 2 B1: the cell machinery. PERSISTENT references (no cell)
    // are spawned once here — the player; everything celled streams in
    // and out through the CellStreamer (update()).
    worldModel = world::WorldModel::build(forms);
    cellLoader = std::make_unique<world::CellLoader>(
        world, forms, worldModel, spawner, categories);
    cellStreamer = std::make_unique<world::CellStreamer>(*cellLoader,
                                                         worldModel, forms);
    // Chantier 5 B4: the pending save layer remembers unloaded cells
    // (capture before unload, spawn veto for disabled references). Fresh
    // per scene enter — a loaded save carries its state in `forms`.
    saveController.pending().clear();
    cellLoader->beforeUnload = [this](data::FormHandle,
                                      ecs::Entity cellEntity) {
        saveController.pending().captureCell(world, forms, cellEntity,
                                             gameTags);
    };
    cellLoader->spawnFilter = [this](const core::Guid& referenceId) {
        return saveController.pending().isEnabled(referenceId);
    };
    overworldHandle = data::FormHandle {};
    if (const auto* overworld =
            data::findByEditorId<world::WorldspaceForm>(forms, "Overworld")) {
        overworldHandle = forms.handleOf(overworld->id);
    } else {
        LOG_WARN("chantier 2 B1: no Overworld worldspace — nothing streams");
    }
    activeWorldspace = overworldHandle;
    interiorMode = false;
    interaction.reset();
    // Chantier 3 B1: start the day at 10:00, ~7.5 real minutes per game
    // hour (timescale 12 — "Animate" boosts it).
    gameClock = gameplay::GameClock {};
    gameClock.gameSeconds = 10.0 * 3600.0;
    gameClock.timescale = 12.0f;
    // Chantier 5 B5: the WorldStateForm of a loaded save overrides the
    // fresh-game defaults (clock, worldspace; the camera is restored at
    // the end of onEnter, after the start-spot heuristic).
    loadedWorldState.reset();
    if (saveController.loadedFromSave()) {
        data::forEach<gameplay::WorldStateForm>(
            forms, [&](const gameplay::WorldStateForm& form) {
                loadedWorldState = form;
            });
    }
    if (loadedWorldState) {
        gameClock.gameSeconds = loadedWorldState->gameSeconds;
        gameClock.timescale = loadedWorldState->timescale;
        if (loadedWorldState->activeWorldspace.isValid()) {
            const data::FormHandle handle =
                forms.handleOf(loadedWorldState->activeWorldspace);
            if (handle.isValid()) {
                activeWorldspace = handle;
                if (const auto* space =
                        static_cast<const world::WorldspaceForm*>(
                            forms.get(handle))) {
                    interiorMode = space->interior;
                }
            }
        }
    }
    // B3/B4: a fresh edit session over the freshly resolved database.
    levelEditor = std::make_unique<LevelEditor>(forms, formTypes);
    mode = SceneMode::Spectator; // fresh on (re-)enter; Play set later if a save
    sceneEditor.deselect();
    createConsole(); // chantier 4 B7: F8 in-game dev console
}

void LandscapeScene::spawnInitialWorld(rhi::Device& device) {
    world::SpawnContext spawnCtx { world, forms, categories };
    const auto* playerForm =
        data::findByEditorId<data::ActorForm>(forms, "Player");
    playerEntity = ecs::Entity {};
    u32 persistent = 0;
    data::forEach<world::ReferenceForm>(
        forms, [&](const world::ReferenceForm& reference) {
            if (!reference.enabled || reference.prefab.isValid() ||
                reference.cell.isValid()) {
                return; // celled refs belong to the streamer
            }
            const ecs::Entity entity =
                spawner.spawn(spawnCtx, reference, ecs::Entity {});
            if (entity.is_alive()) {
                ++persistent;
                if (playerForm && reference.baseForm == playerForm->id) {
                    playerEntity = entity;
                }
            }
        });
    if (playerEntity.is_alive()) {
        // Chantier 5 B3: the shared post-spawn seam (stats, then saved
        // state OR loadout). The starting kit only exists on a fresh game.
        const bool fromSave =
            finalizeActorSpawn(playerEntity,
                               playerForm ? playerForm->id : core::Guid {});
        if (!fromSave && playerWeapon) {
            // Chantier 4 B3: the sword really sits in the bag, equipped.
            auto& bag = playerEntity.get_mut<gameplay::Inventory>();
            if (gameplay::itemCount(bag, playerWeapon->id) == 0) {
                gameplay::addItem(bag, playerWeapon->id, 1);
            }
            playerEntity.get_mut<gameplay::Equipment>().weapon =
                playerWeapon->id;
        }
        // A4/D2: re-mirror a loaded quest log + bounty onto the player.
        questDirector.syncQuestTags(makeQuestContext());
        questDirector.syncWantedTag(makeQuestContext());
    } else {
        LOG_WARN("B5.5: no Player actor spawned — controller falls back to "
                 "fixed speeds");
    }
    LOG_INFO("B1 (ch.2): {} persistent reference(s); cells stream around "
             "the player",
             persistent);

    // B6: Forms-driven NPCs — every spawned actor whose ActorForm resolves
    // an ActorVisual gets its GPU skin, its data-built locomotion graph,
    // and its patrol brain. The scene builds no character by hand anymore.
    shaders->load("skinned",
                  { { "FrameUbo", 0 }, { "ModelUbo", 1 },
                    { "LightsUbo", 5 } },
                  { { "uAlbedo", 0 } });
    // The skinned pipeline is built lazily by NpcDirector::draw on first use.

    // Initial cell ring around the player spawn, then the post-spawn
    // fixups (ground snap, NPC build) — the same pair update() re-runs
    // whenever the ring changes.
    Vec3 startFocus { 32.0f, 0.0f, 368.0f };
    if (playerEntity.is_alive()) {
        startFocus = playerEntity.get<world::Transform>().position;
    }
    if (activeWorldspace.isValid()) { // a loaded save may start indoors
        cellStreamer->update(activeWorldspace, startFocus.x, startFocus.z);
    }
    const StreamingContext sctx = makeStreamingContext();
    streaming.snapCellEntities(sctx);
    refreshNpcs(device);
    streaming.refreshNavObstacles(sctx);

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

    // Start beside the NPC (slightly above, looking at it) — never
    // inside the terrain: the spot is grounded on the SAME height function
    // the mesh uses. Fallback: safely above the demo area.
    if (!npcDirector.npcs().empty()) {
        const Vec3 characterSpot = npcDirector.characterSpot();
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

    // Chantier 5 B5: a loaded game resumes where it stood — camera on the
    // player, saved look angles, straight into Play (no boot menu). The
    // capsule spawns at the SAVED position directly (the travel pattern —
    // enterPlayMode would re-ground on the terrain, wrong indoors).
    if (saveController.loadedFromSave()) {
        Vec3 feet = flyCamera.camera.position - Vec3 { 0.0f, 1.7f, 0.0f };
        if (playerEntity.is_alive()) {
            feet = playerEntity.get<world::Transform>().position;
        }
        flyCamera.camera.position = feet + Vec3 { 0.0f, 1.7f, 0.0f };
        if (loadedWorldState) {
            flyCamera.camera.yaw = loadedWorldState->playerYaw;
            flyCamera.camera.pitch = loadedWorldState->playerPitch;
        }
        screenStack.close("mainmenu");
        syncScreens();
        if ((!loadedWorldState || loadedWorldState->playMode) && physics) {
            playerController.spawnBody(*physics,
                                       feet + Vec3 { 0.0f, 0.25f, 0.0f });
            mode = SceneMode::Play;
            engine->getWindow().setRelativeMouseMode(true);
            screenStack.show("hud");
            syncScreens();
        }
    }

    // Dev convenience (interior lighting tuning, 2026-07-07): boot the
    // session INSIDE the house — the pending travel rides the normal
    // door fade and fires once the main menu closes (Enter the world /
    // Escape). Flip to false to boot in the village again.
    constexpr bool kDevStartInterior = false; // exterior pass next (dev)
    if (kDevStartInterior && !saveController.loadedFromSave()) {
        // HouseDoorExterior's arrival marker (village.toml, the marker
        // REFERENCE inside the interior cell).
        if (const auto marker = core::Guid::fromString(
                "4d7a9b30-0000-4000-8000-000000000023");
            marker && forms.find<world::ReferenceForm>(*marker)) {
            interaction.beginTravel(*marker);
        }
    }
}

void LandscapeScene::onExit() {
    rhi::Device& device = engine->getDevice();
    engine->getWindow().setRelativeMouseMode(false);
    engine->getWindow().setTextInput(false);
    // Chantier 4: game UI (one UiSystem per process — release before any
    // other scene creates its own).
    if (uiCreated) {
        uiSystem.destroy(device);
        uiCreated = false;
    }
    screenStack = ScreenStack {};
    shownScreens.clear();
    uiModalWasOpen = false;
    uiTextInputOn = false;
    questDirector.reset(); // runner/log/demo-quest point into `forms`
    hud.reset(); // dialogue options point into `forms` too
    uiRouter.reset(); // open container/vendor die with the world
    goldForm = nullptr;
    sceneConsole.reset(); // panel/VM/session reference forms — before re-resolve
    destroyOffscreenTarget(device);
    device.destroyPipeline(blitPipeline);
    device.destroySampler(blitSampler);
    // B1 mesh path: per-entry draw state, then the caches (their dtors free
    // the GPU resources they own — device is alive here).
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
    snapshot = RenderSnapshot {};
    meshCache.reset();
    materialTextures.reset();
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
    // B6 NPCs: GPU state per NPC, the pipeline, and the CPU-side rig cache.
    npcDirector.teardown(device);
    // Chantier 2 B1: cell machinery (references scene members — release
    // before the members are reset on the next onEnter).
    cellStreamer.reset();
    cellLoader.reset();
    overworldHandle = data::FormHandle {};
    // B4/B5 physics: bodies -> tiles -> world (each references the previous).
    mode = SceneMode::Spectator;
    playerController.destroyBody();
    debugCapsule.reset();
    streaming.reset(physics.get());
    vegCollision.reset();
    terrainCollision.reset();
    physics.reset();
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

void LandscapeScene::rebuildBlitPipeline(rhi::Device& device) {
    if (blitPipeline.id != 0) {
        device.destroyPipeline(blitPipeline);
    }
    blitPipeline =
        device.createPipeline({ .shader = shaders->get(kTonemapShader) });
    blitShaderGeneration = shaders->generation(kTonemapShader);
}

void LandscapeScene::update(f32 dt) {
    frameProbe.beginFrame(); // ends in render() — one probe per frame
    timeSeconds += dt;
    // B1 mesh path: pump async residency (worker decodes -> main-thread
    // uploads, §7), then extract this frame's snapshot from the world.
    {
        core::FrameProbe::Scope probe { frameProbe, "assets" };
        if (materialTextures) {
            materialTextures->pumpUploads();
        }
        if (meshCache) {
            meshCache->pumpUploads();
        }
    }
    // Chantier 4 B2: the game UI runs first — an open MODAL screen pauses
    // the sim below (UiScreenForm.modal) and owns mouse/keyboard; the HUD
    // overlay just reads. Dev ImGui stays live either way.
    {
        core::FrameProbe::Scope probe { frameProbe, "gameUi" };
        updateGameUi(dt);
    }
    const bool uiPaused = uiCreated && screenStack.modalOpen();
    // Spectator freezes the *living* sim — physics, the game clock (so the
    // sun stops too), the character tick and the NPCs — while the free camera
    // keeps roaming and the world keeps streaming and rendering. That frozen-
    // moment-you-can-look-around is the base for a future photo mode. A modal
    // UI still pauses everything, camera included (it owns the input).
    const bool simPaused = uiPaused || (mode == SceneMode::Spectator);
    // B4/B5: physics tick + collision tiles around the focus (the player
    // in Play mode, the camera in Fly); the debug capsule free-falls.
    if (physics && !simPaused) {
        core::FrameProbe::Scope probe { frameProbe, "physics" };
        physics->tick(dt);
        if (!interiorMode) { // interiors have no terrain to collide with
            const Vec3 focus =
                (mode == SceneMode::Play) && playerController.body()
                    ? playerController.body()->position()
                    : flyCamera.camera.position;
            terrainCollision->update(focus);
            vegCollision->update(focus); // trunks + rocks (dev report)
        }
        if (debugCapsule) {
            debugCapsule->move({ 0.0f, 0.0f, 0.0f }, dt);
        }
    }
    // Chantier 2 B1: stream cells around the focus; on any ring change,
    // re-run the post-spawn fixups (idempotent snap + NPC refresh).
    if (cellStreamer && activeWorldspace.isValid() && !uiPaused) {
        core::FrameProbe::Scope probe { frameProbe, "cells" };
        const Vec3 focus =
            (mode == SceneMode::Play) && playerController.body()
                ? playerController.body()->position()
                : flyCamera.camera.position;
        // Chantier 5 B8: border crossings spread their spawns — one cell
        // per frame (the initial ring and travels load whole, behind the
        // fade). Fixups are idempotent, re-run per loaded cell.
        if (cellStreamer->update(activeWorldspace, focus.x, focus.z, 2, 3,
                                 /*maxLoads=*/1)) {
            const StreamingContext sctx = makeStreamingContext();
            streaming.snapCellEntities(sctx);
            refreshNpcs(engine->getDevice());
            streaming.refreshNavObstacles(sctx);
        }
    }
    {
        // B2: bodies follow spawns + mesh residency.
        core::FrameProbe::Scope probe { frameProbe, "colliders" };
        streaming.updateStaticColliders(makeStreamingContext());
    }
    // The sky follows the clock UNCONDITIONALLY — it is presentation, not
    // sim. Gating it on !uiPaused made the wait menu look broken: +8 h
    // from the pause chain landed in gameSeconds, but the world behind
    // the (still open) pause menu kept the stale sun.
    sky.timeOfDay =
        static_cast<f32>(std::fmod(gameClock.gameHours(), 24.0));
    if (!simPaused) {
        // B7/ch.3 B1: interaction prompts + the travel fade state machine.
        interaction.update(dt, makeInteractionContext());
        // Chantier 3 B1: the game clock owns time — advancing it stays
        // paused with the sim; tickCharacter gets REAL game-seconds
        // (regen/survival at timescale).
        gameClock.timescale = animateTime ? 720.0f : 12.0f;
        const f64 gameDt = gameClock.advance(dt);
        if (playerEntity.is_alive()) {
            core::FrameProbe::Scope probe { frameProbe, "charTick" };
            const gameplay::CharacterTickContext tickCtx { derivedStats,
                                                           gameTags,
                                                           statsTuning };
            // Chantier 4 B3: equipped gear folds into the derived stats.
            gameplay::StatModifiers equipMods;
            if (playerEntity.has<gameplay::Equipment>()) {
                gameplay::applyEquipmentModifiers(
                    playerEntity.get<gameplay::Equipment>(), forms,
                    equipMods);
            }
            // C3: encumbrance penalties fold into the same channel. The
            // max reads last frame's current (one frame of lag is fine).
            if (playerEntity.has<gameplay::Inventory>()) {
                playerCarriedWeight = gameplay::inventoryWeight(
                    forms, playerEntity.get<gameplay::Inventory>());
                const f32 maxEncumbrance = gameplay::currentValueOf(
                    playerEntity.get<gameplay::AbilitySystem>(),
                    gameplay::attr("maxEncumbrance"));
                playerEncumbrance = gameplay::encumbranceCategory(
                    playerCarriedWeight, maxEncumbrance);
                gameplay::encumbranceModifiers(playerEncumbrance, equipMods);
            }
            gameplay::tickCharacter(playerEntity, dt, gameDt, tickCtx,
                                    equipMods);
        }
    }
    {
        core::FrameProbe::Scope probe { frameProbe, "extract" };
        snapshot.meshes.clear();
        extractMeshes(world, snapshot);
    }
    if (debugCapsule) {
        // Visualize as the residency placeholder box (magenta), stretched
        // to the capsule's stance, standing at the FEET position.
        const Mat4 transform =
            glm::scale(glm::translate(Mat4 { 1.0f }, debugCapsule->position()),
                       Vec3 { 0.9f, 2.25f, 0.9f });
        snapshot.meshes.push_back({ core::Guid {}, core::Guid {}, transform });
    }
    // B6: patrol + graph-driven poses for every Forms-built NPC.
    if (!simPaused) {
        core::FrameProbe::Scope probe { frameProbe, "npcs" };
        updateNpcs(dt);
    }
    // Wind phase integrates the CURRENT strength: speed changes bend the
    // drift/sway smoothly instead of teleporting the pattern.
    windTime += dt * glm::max(atmos.windStrength, 0.05f);

    // Weather crossfade (owned by WeatherController): slides `atmos` from the
    // captured start state to the selected weather over its duration.
    weather.update(atmos, dt);

    // Mode switching lives with the F2/F3 hotkeys (drawn overlay); Play is
    // home. Nothing to toggle here anymore.
    if (uiPaused) {
        // A modal screen owns the input; cameras and player hold still.
    } else if (sceneConsole.visible()) {
        // The dev console owns the keyboard; the player / camera hold still
        // (so WASD types instead of walking) while the sim keeps ticking.
    } else if ((mode == SceneMode::Play) && playerController.body()) {
        playerController.update(dt, makePlayerContext());
    } else {
        // Don't steal the mouse from ImGui: clicking a panel must not
        // mouselook. Spectator looks like Play (mouselook always, no button);
        // Edit looks only while RMB is held, keeping LMB free for pick /
        // place / sculpt. Hold Alt to free the cursor (reach ImGui panels)
        // without leaving the mode.
        const bool freeMouse = ImGui::GetIO().KeyAlt;
        const bool allowCapture =
            !ImGui::GetIO().WantCaptureMouse && !freeMouse;
        const auto trigger =
            (mode == SceneMode::Edit)
                ? render::FlyCamera::LookTrigger::RightButton
                : render::FlyCamera::LookTrigger::Always;
        flyCamera.update(engine->getInput(), engine->getWindow(), dt,
                         allowCapture, trigger);
    }
    // (Time-of-day now advances through the game clock, above.)

    // Remember the active gameplay mode (outside menus) so a menu returns here
    // on Escape. Defaults to Play, so the boot main menu closes into Play.
    if (!(uiCreated && screenStack.modalOpen())) {
        lastActiveMode = mode;
    }

    // Chantier 5 B5: a requested load re-enters the scene with the save
    // resolved as the last layer. End of update: nothing touches the
    // world after this.
    if (saveController.takeReloadRequest()) {
        onExit();
        onEnter();
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
    playerController.spawnBody(*physics, feet);
    mode = SceneMode::Play;
    // Play owns the keyboard: drop any lingering ImGui nav focus. With
    // NavEnableKeyboard, WantCaptureKeyboard stays true as long as a panel
    // holds nav focus (one click in the Edit-mode panels is enough), and the
    // captured mouse makes clicking the void to release it impossible — so
    // Escape/I/T/J went dead after an F3 round-trip (dev report 2026-07-09).
    ImGui::SetWindowFocus(nullptr);
    engine->getWindow().setRelativeMouseMode(true);
    screenStack.show("hud"); // the HUD overlay lives with Play mode
}

void LandscapeScene::exitPlayMode() {
    mode = SceneMode::Spectator;
    playerController.destroyBody();
    engine->getWindow().setRelativeMouseMode(false);
    screenStack.close("hud");
    // The camera stays where the player stood — Fly resumes from there.
}

// Drive the scene into `target` from the current mode, reusing enter/exitPlay-
// Mode so the capsule and mouse/HUD state stay consistent. Used to restore the
// pre-menu mode on Escape (and anywhere a mode needs setting programmatically).
void LandscapeScene::restoreMode(SceneMode target) {
    if (mode == target) {
        return;
    }
    switch (target) {
    case SceneMode::Play:
        enterPlayMode(); // spawns the capsule, grabs the mouse, shows the HUD
        break;
    case SceneMode::Spectator:
        if (mode == SceneMode::Play) {
            exitPlayMode(); // -> Spectator (tears down the capsule)
        } else {
            mode = SceneMode::Spectator; // from Edit
        }
        break;
    case SceneMode::Edit:
        if (mode == SceneMode::Play) {
            exitPlayMode();
        }
        mode = SceneMode::Edit;
        break;
    }
}

// --- B3/B4: the level editor -------------------------------------------------------

// mouseRayDirection / pickEntity / groundUnderMouse moved to SceneEditor (U4-5).

// --- B9: terrain sculpt (extracted to TerrainSculptTool, audit U4-5) ---------------

// The scene half of the sculpt contract: read access to the terrain height /
// current overlay, plus the publish side effects the tool can't own — swap the
// immutable overlay in, rebuild terrain/scatter/collision, invalidate occlusion
// and re-snap cell entities. Rebuilt each frame (cheap: refs + one closure).
SculptContext LandscapeScene::makeSculptContext() {
    return SculptContext {
        terrain.params,
        heightPatches.get(),
        forms,
        *levelEditor,
        [this](std::shared_ptr<render::HeightPatches> next,
               const std::vector<u64>& changed, bool commit) {
            // Show the new heights immediately: swap the live overlay and queue
            // a terrain re-mesh of just the changed chunks (deferred to the
            // safe point in render(), seamless swap). This runs every preview
            // frame during a stroke.
            terrain.params.patches = next;
            sculptDirtyChunks.insert(sculptDirtyChunks.end(), changed.begin(),
                                     changed.end());
            if (!commit) {
                return; // live preview: terrain only, no heavy churn
            }
            // Stroke release — the permanent publish: the overlay becomes the
            // committed one, grass/veg re-scatter onto the new heights, and
            // collision / cell snap rebuild.
            heightPatches = next;
            sculptScatterChunks.insert(sculptScatterChunks.end(),
                                       changed.begin(), changed.end());
            occlusion.invalidate();
            terrainCollision = std::make_unique<TerrainCollision>(
                *physics, terrain.params, &engine->getJobSystem());
            vegCollision = std::make_unique<VegetationCollision>(
                *physics, terrain.params);
            streaming.snapCellEntities(makeStreamingContext());
        }
    };
}

// Bundle the editor's systems for SceneEditor this frame — references into the
// scene plus the sculpt sub-contract. Rebuilt each frame (cheap: refs + one
// closure). SceneEditor owns the editor state and the interaction/UI.
EditorContext LandscapeScene::makeEditorContext() {
    return EditorContext {
        flyCamera,       world,           *meshCache,      interiorMode,
        terrain.params,  forms,           *levelEditor,    activeWorldspace,
        worldModel,      categories,      *cellLoader,     spawner,
        makeSculptContext(),
    };
}

// --- B7: doors & worldspace travel ------------------------------------------------

// Bundle the scene systems the [E] interaction touches for
// InteractionController this frame — references into the scene plus the
// actions that stay scene territory as closures (travel swaps the
// worldspace; dialogue/container/screens are UI wiring). Rebuilt each call
// (cheap: refs + four closures). Mirrors makeEditorContext.
InteractionContext LandscapeScene::makeInteractionContext() {
    return InteractionContext {
        forms,
        doorQuery,
        interactQuery,
        npcDirector.npcs(),
        engine->getInput(),
        gameClock,
        statsTuning,
        saveController.pending(),
        physics.get(),
        playerController.body(),
        playerEntity,
        flyCamera.camera.forward(),
        mode == SceneMode::Play,
        [this](const core::Guid& target) { performTravel(target); },
        [this](ecs::Entity partner, const core::Guid& dialogue) {
            questDirector.setDialoguePartner(partner); // the vendor for B5
            questDirector.openDialogue(makeQuestContext(), dialogue);
        },
        [this](ecs::Entity container) {
            uiRouter.openContainerScreen(makeUiRouterContext(), container);
        },
        [this](const str& screen) {
            if (!screenStack.find(screen)) {
                return false;
            }
            screenStack.show(screen);
            return true;
        },
    };
}

void LandscapeScene::performTravel(const core::Guid& targetReference) {
    const auto* marker = forms.find<world::ReferenceForm>(targetReference);
    if (!marker) {
        LOG_WARN("B7: travel target {} not found",
                 targetReference.toString());
        return;
    }
    const auto* cellForm = forms.find<world::CellForm>(marker->cell);
    if (!cellForm) {
        LOG_WARN("B7: travel target {} has no cell",
                 targetReference.toString());
        return;
    }
    const data::FormHandle space = forms.handleOf(cellForm->worldspace);

    // Swap worldspaces: drop everything streamed, ring around the arrival,
    // then the usual post-spawn fixups. Colliders for despawned entities
    // fall out on the next updateStaticColliders pass.
    cellStreamer->unloadAll();
    activeWorldspace = space;
    interiorMode = cellForm->interior;
    cellStreamer->update(activeWorldspace, marker->position.x,
                         marker->position.z);
    const StreamingContext sctx = makeStreamingContext();
    streaming.snapCellEntities(sctx);
    refreshNpcs(engine->getDevice());
    streaming.updateStaticColliders(sctx);
    streaming.refreshNavObstacles(sctx);

    // Fresh terrain tiles for the new space (none are built in interiors).
    terrainCollision = std::make_unique<TerrainCollision>(
        *physics, terrain.params, &engine->getJobSystem());
    vegCollision =
        std::make_unique<VegetationCollision>(*physics, terrain.params);

    // Teleport the capsule to the marker, facing its authored yaw. The
    // fade-in (0.3 s) covers the async floor-collider cook — the player
    // doesn't move (and barely falls) until it lands. In Fly (dev
    // camera, no capsule) the camera alone travels — an interior with
    // the camera still outside is a black screen.
    if (playerController.body()) {
        playerController.spawnBody(
            *physics, marker->position + Vec3 { 0.0f, 0.25f, 0.0f });
    }
    flyCamera.camera.yaw =
        2.0f * std::atan2(marker->rotation.y, marker->rotation.w);
    flyCamera.camera.pitch = 0.0f;
    flyCamera.camera.position =
        marker->position + Vec3 { 0.0f, 1.95f, 0.0f };
    LOG_INFO("B7: traveled to {} ({}), interior = {}",
             cellForm->editorId,
             marker->position.x, interiorMode);
}

// --- Chantier 4: the RmlUi game UI --------------------------------------------------

void LandscapeScene::createGameUi(rhi::Device& device) {
    // Document roots = every plugin's ui/ dir, in load order (last wins —
    // a mod overrides a screen by shipping the same path).
    vector<std::filesystem::path> roots;
    for (const data::Plugin& plugin : pluginStack.plugins) {
        const auto root = std::filesystem::path { plugin.baseDir } / "ui";
        if (std::filesystem::exists(root) &&
            std::find(roots.begin(), roots.end(), root) == roots.end()) {
            roots.push_back(root);
        }
    }
    if (roots.empty()) {
        roots.push_back(platform::executableDir() / "data" / "base" / "ui");
    }
    const auto fontRoots = roots; // create() moves the list
    uiCreated = uiSystem.create(device, *shaders, roots,
                                static_cast<u32>(engine->getWindow().width()),
                                static_cast<u32>(engine->getWindow().height()));
    if (!uiCreated) {
        LOG_WARN("Game UI unavailable (UiSystem creation failed)");
        return;
    }
    // Fonts before documents (RmlUi requirement): every plugin may add
    // faces under ui/fonts/.
    for (const auto& root : fontRoots) {
        const auto fontsDir = root / "fonts";
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::directory_iterator { fontsDir, ec }) {
            if (entry.path().extension() == ".ttf" ||
                entry.path().extension() == ".otf") {
                uiSystem.loadFont(entry.path());
            }
        }
    }

    // Data models BEFORE the documents that reference them (Rml freezes
    // bindings at creation). One "hud" model for B2; screens add theirs.
    uiSystem.createModel(
        { .name = "hud",
          .numbers = { "healthPct", "energyPct", "essencePct",
                       "posturePct" },
          .strings = { "healthText", "energyText", "essenceText", "clock",
                       "prompt", "talk" },
          .bools = { "promptVisible", "talkVisible" },
          .rows = true }); // B7: nameplates over hostile/hurt NPCs
    // B3: the player-side item table (inventory screen + the player panel
    // of the container/barter screens) and the loot side.
    uiSystem.createModel(
        { .name = "inventory",
          .strings = { "search", "detailName", "detailInfo", "weightText",
                       "equipLabel", "goldText" },
          .bools = { "hasSelection", "selUsable", "transferMode" },
          .rows = true,
          .events = { "tab", "sortCol", "pick", "equipAction",
                      "useAction" } });
    uiSystem.createModel({ .name = "container",
                           .strings = { "title" },
                           .rows = true,
                           .events = { "pickLoot", "takeAll" } });
    // B4: dialogue — the NPC line + the player options as rows.
    uiSystem.createModel({ .name = "dialogue",
                           .strings = { "npcName", "npcLine" },
                           .rows = true,
                           .events = { "choose" } });
    // B5: barter — the vendor side (the player side reuses "inventory").
    uiSystem.createModel(
        { .name = "barter",
          .strings = { "title", "playerGold", "vendorGold" },
          .rows = true,
          .events = { "pickBuy" } });
    // B6: one shared model for the menu screens (pause/main/wait/workshop).
    uiSystem.createModel({ .name = "menu",
                           .strings = { "clockLine" },
                           .events = { "menuAction" } });
    // Chantier 5 B6: the saves-list screen (rows: name + timestamp).
    uiSystem.createModel({ .name = "saves",
                           .bools = { "empty" },
                           .rows = true,
                           .events = { "loadSlot", "loadCancel" } });
    // Chantier 6 A3: the quest journal (rows: quest header + task lines).
    uiSystem.createModel({ .name = "journal",
                           .bools = { "empty" },
                           .rows = true,
                           .events = { "journalClose" } });
    uiSystem.setModelEventHandler(
        [this](const str& model, const str& event, const vector<str>& args) {
            uiRouter.handleUiEvent(makeUiRouterContext(), model, event, args);
        });

    // Screens from UiScreenForm records — pure data, moddable (§5).
    data::forEach<data::UiScreenForm>(forms, [&](const data::UiScreenForm& f) {
        screenStack.define({ .name = f.screen,
                             .document = f.document,
                             .modal = f.modal,
                             .overlay = f.overlay });
    });
    // Preload every screen document once (then hide them): binding or
    // syntax errors in a base or MODDED document surface in the log at
    // startup instead of at first open.
    for (const ScreenStack::Screen* screen : screenStackPreloadList()) {
        screenStack.show(screen->name);
    }
    syncScreens();
    screenStack.closeAll();
    screenStack.close("hud");
    // B6: boot into the main menu — "Enter the world" starts Play;
    // Escape dismisses it for the dev tools (Fly camera, panels).
    if (screenStack.find("mainmenu")) {
        hud.updateMenuClockLine(makeHudContext());
        screenStack.show("mainmenu");
    }
    syncScreens();
}

// Every defined screen, for the startup preload pass.
vector<const ScreenStack::Screen*> LandscapeScene::screenStackPreloadList()
    const {
    vector<const ScreenStack::Screen*> screens;
    data::forEach<data::UiScreenForm>(forms, [&](const data::UiScreenForm& f) {
        if (const ScreenStack::Screen* screen = screenStack.find(f.screen)) {
            screens.push_back(screen);
        }
    });
    return screens;
}

void LandscapeScene::updateGameUi(f32 dt) {
    if (!uiCreated) {
        return;
    }
    platform::Input& input = engine->getInput();
    const bool imguiOwnsKeys = ImGui::GetIO().WantCaptureKeyboard;

    // Tab: back/close the top screen (dev request 2026-07-07 — the
    // Skyrim reflex). Escape only drives the pause/main menus below.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() &&
        input.wasPressed(platform::Key::Tab)) {
        screenStack.closeTop();
    }
    // Escape: toggle the pause menu in Play (and still dismiss the boot
    // main menu for the dev tools); Fly/Edit keep Escape free for tools.
    if (!imguiOwnsKeys && input.wasPressed(platform::Key::Escape)) {
        const ScreenStack::Screen* top = screenStack.topModal();
        if (top && (top->name == "pause" || top->name == "mainmenu")) {
            screenStack.closeTop();
            // Escape returns to the mode in use before the menu — Play on a
            // fresh boot (the main menu sits over the default), or whatever the
            // player last switched to.
            restoreMode(lastActiveMode);
        } else if (!screenStack.modalOpen() && (mode == SceneMode::Play) &&
                   screenStack.find("pause")) {
            hud.updateMenuClockLine(makeHudContext());
            screenStack.show("pause");
        }
    }
    // I: toggle the inventory (B3) — not while typing in a text field.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() &&
        input.wasPressed(platform::Key::I)) {
        const ScreenStack::Screen* top = screenStack.topModal();
        if (top && (top->name == "inventory" || top->name == "container")) {
            screenStack.closeTop();
        } else if (!screenStack.modalOpen() && playerEntity.is_alive()) {
            uiRouter.openInventoryScreen(makeUiRouterContext());
        }
    }
    // T: the wait menu (B6) — Play only, nothing else open.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() && (mode == SceneMode::Play) &&
        input.wasPressed(platform::Key::T) && !screenStack.modalOpen()) {
        hud.updateMenuClockLine(makeHudContext());
        screenStack.show("wait");
    }
    // J: the quest journal (chantier 6 A3) — the I-key idiom.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() &&
        input.wasPressed(platform::Key::J)) {
        const ScreenStack::Screen* top = screenStack.topModal();
        if (top && top->name == "journal") {
            screenStack.closeTop();
        } else if (!screenStack.modalOpen()) {
            hud.pushJournalModel(makeHudContext());
            screenStack.show("journal");
        }
    }

    const bool modal = screenStack.modalOpen();
    if (modal != uiModalWasOpen) {
        // A modal frees the mouse (and pauses the sim, handled in
        // update()); closing it restores the Play capture.
        if ((mode == SceneMode::Play)) {
            engine->getWindow().setRelativeMouseMode(!modal);
        }
        uiModalWasOpen = modal;
    }
    if (modal) {
        // The open screen owns the input.
        const Vec2 mouse = input.mousePosition();
        uiSystem.processMouseMove(static_cast<i32>(mouse.x),
                                  static_cast<i32>(mouse.y));
        if (input.mousePressed(platform::MouseButton::Left)) {
            uiSystem.processMouseButton(0, true);
        }
        if (input.mouseReleased(platform::MouseButton::Left)) {
            uiSystem.processMouseButton(0, false);
        }
        if (input.wheelDelta() != 0.0f) {
            uiSystem.processMouseWheel(input.wheelDelta());
        }
        for (const platform::Input::KeyEvent& event : input.keyEvents()) {
            if (event.key == platform::Key::Escape ||
                event.key == platform::Key::Tab) {
                continue; // handled above (stack pop / pause) — and Tab
                          // must not ALSO move the RmlUi focus
            }
            uiSystem.processKey(event.key, event.down);
        }
        uiSystem.processTextInput(input.textInput());
    }
    // OS text events only while an Rml text field holds the focus.
    const bool wantText = modal && uiSystem.textFieldFocused();
    if (wantText != uiTextInputOn) {
        engine->getWindow().setTextInput(wantText);
        uiTextInputOn = wantText;
    }
    // Two-way search box: Rml writes into the bound slot, we mirror it
    // into the view when it changes (B3). Only the item screens carry it.
    if (modal) {
        const ScreenStack::Screen* top = screenStack.topModal();
        if (top && (top->name == "inventory" || top->name == "container" ||
                    top->name == "barter")) {
            const str search = uiSystem.getString("inventory", "search");
            if (search != hud.inventory().search()) {
                hud.inventory().setSearch(search);
                hud.pushItemModels(makeHudContext());
            }
        }
    }

    hud.updateHudModel(makeHudContext());
    syncScreens();
    uiSystem.update(dt);
}

// Bundle the scene systems the RmlUi presenter reads for GameHud this frame
// — references into the scene plus a few scalars (audit U4-9). Rebuilt each
// call (cheap). Mirrors makeEditorContext / makeInteractionContext.
HudContext LandscapeScene::makeHudContext() {
    return HudContext {
        uiSystem,
        uiCreated,
        forms,
        playerEntity,
        gameClock,
        interaction,
        mode == SceneMode::Play,
        flyCamera,
        static_cast<f32>(engine->getWindow().width()),
        static_cast<f32>(engine->getWindow().height()),
        playerController.body(),
        npcDirector.npcs(),
        uiRouter.containerEntity(),
        uiRouter.barterMode(),
        uiRouter.vendorBuyMult(),
        uiRouter.vendorSellMult(),
        goldForm,
        questDirector.questLog(),
        questDirector.dialogueRunner(),
        makeEvalContext(),
        screenStack,
    };
}

void LandscapeScene::syncScreens() {
    if (!uiCreated) {
        return;
    }
    vector<str> want;
    for (const ScreenStack::Screen* screen : screenStack.visibleScreens()) {
        want.push_back(screen->document);
    }
    for (const str& doc : shownScreens) {
        if (std::find(want.begin(), want.end(), doc) == want.end()) {
            uiSystem.closeDocument(doc);
        }
    }
    for (const str& doc : want) {
        if (std::find(shownScreens.begin(), shownScreens.end(), doc) ==
            shownScreens.end()) {
            if (!uiSystem.showDocument(doc)) {
                LOG_WARN("UI screen document '{}' failed to load", doc);
            }
        }
    }
    shownScreens = std::move(want);
}

// --- Chantier 5: the post-spawn seam -------------------------------------------------

// Snapshot the scene state SaveController serializes for this save (audit
// U4-1): references plus the world state the WorldStateForm records and the
// two scene actions the save needs as closures (sweeping the live references
// and the toast). Rebuilt per save (cheap). Mirrors the other make*Context
// builders.
SaveContext LandscapeScene::makeSaveContext() {
    return SaveContext {
        forms,
        formTypes,
        gameTags,
        questDirector.questLog(),
        gameClock,
        activeWorldspace,
        flyCamera.camera.yaw,
        flyCamera.camera.pitch,
        mode == SceneMode::Play,
        weather.selected(),
        // Inline capture is safe: captureEntity reads components and writes
        // into the pending layer's own map — it mutates no ECS structure
        // (SaveGame.cpp), so no iterator invalidation during each.
        [this](const std::function<void(ecs::Entity)>& fn) {
            interactQuery.each([&](flecs::entity e, const world::Transform&,
                                   const world::RefId&) {
                fn(ecs::Entity { e });
            });
        },
        [this](const str& msg) { interaction.say(msg, 3.0f); },
    };
}

bool LandscapeScene::finalizeActorSpawn(ecs::Entity entity,
                                        const core::Guid& actorFormId) {
    const gameplay::CharacterTickContext tickCtx { derivedStats, gameTags,
                                                   statsTuning };
    gameplay::initializeActorStats(entity, tickCtx);
    if (!entity.has<gameplay::Inventory>()) {
        entity.set<gameplay::Inventory>({});
    }
    if (!entity.has<gameplay::Equipment>()) {
        entity.set<gameplay::Equipment>({});
    }
    // D1/D2: present on every actor BEFORE the saved-state apply, so the
    // name-matched SavedStatsForm fields (bounty, lastRestockHours) land.
    if (!entity.has<gameplay::Bounty>()) {
        entity.set<gameplay::Bounty>({});
    }
    if (!entity.has<gameplay::VendorState>()) {
        entity.set<gameplay::VendorState>({});
    }
    core::Guid refGuid;
    if (entity.has<world::RefId>()) {
        refGuid = entity.get<world::RefId>().referenceId;
    }
    // Re-apply captured instance overrides: a moved/killed actor keeps the
    // spot it died at instead of snapping back to its authored spawn (the
    // cell loader respawns the resolved record). finalize runs AFTER
    // refreshNpcs grounds the actor's Y, so the captured position wins.
    saveController.pending().applyReferenceOverrides(entity, refGuid);
    // Pending layer first (a cell reloading in THIS session), then the
    // resolved database (a loaded save). The SavedStatsForm existence is
    // the sentinel — a captured actor never re-rolls its loadout (§8).
    if (saveController.pending().hasActorState(refGuid)) {
        gameplay::applySavedState(
            entity, saveController.pending().actorState(refGuid), gameTags);
        return true;
    }
    const gameplay::SavedActorRecords saved =
        gameplay::savedRecordsFor(forms, refGuid);
    if (saved.stats) {
        gameplay::applySavedState(entity, saved, gameTags);
        return true;
    }
    if (actorFormId.isValid()) {
        gameplay::applyLoadout(forms, actorFormId,
                               entity.get_mut<gameplay::Inventory>(),
                               lootRng);
    }
    return false;
}

// --- Chantier 4 B3/B5/B6: UI action routing (UiRouter, audit U4-1) -------------------

// Bundle the scene systems the UI action routing touches for UiRouter this
// dispatch — references plus the scene actions that stay its territory as
// closures (saves, mode flips, waiting, the model pushes that need a
// HudContext). Rebuilt per dispatch (cheap). Mirrors the other
// make*Context builders.
UiRouterContext LandscapeScene::makeUiRouterContext() {
    return UiRouterContext {
        forms,
        uiSystem,
        screenStack,
        hud,
        gameTags,
        statsTuning,
        gameClock,
        lootRng,
        goldForm,
        questDirector.dialogueRunner(),
        playerEntity,
        mode == SceneMode::Play,
        [this] { hud.pushItemModels(makeHudContext()); },
        [this] { hud.pushDialogueModel(makeHudContext()); },
        [this] { hud.updateMenuClockLine(makeHudContext()); },
        [this](const str& slot) {
            saveController.performSave(makeSaveContext(), slot);
        },
        [this](const str& slot) {
            saveController.requestLoad(
                slot, [this](const str& m) { interaction.say(m, 3.0f); });
        },
        [this](f32 hours) {
            interaction.wait(hours, makeInteractionContext());
        },
        [this] { enterPlayMode(); },
        [this] { exitPlayMode(); },
        [this] { engine->requestQuit(); },
    };
}

// --- Chantier 4 B7: console (nameplates -> GameHud, audit U4-9) ----------------------

void LandscapeScene::createConsole() {
    // Infrastructure (session/VM/panel/visibility) lives in SceneConsole;
    // the scene registers the world commands onto its panel (the H2 note:
    // registered by the scene that owns a world; reflection stays the
    // backbone for get/set).
    ConsolePanel& panel = sceneConsole.create(forms, formTypes);
    panel.addCommand("spawn", [this](const str& args) -> str {
        if (args.empty()) {
            return "usage: spawn <EditorId>";
        }
        const data::Form* base = nullptr;
        for (u32 i = 1; i <= forms.count(); ++i) {
            const data::Form* form = forms.get(data::FormHandle { i });
            if (form && form->editorId == args) {
                base = form;
                break;
            }
        }
        if (!base) {
            return "no form named '" + args + "'";
        }
        Vec3 forward = flyCamera.camera.forward();
        forward.y = 0.0f;
        if (glm::dot(forward, forward) < 1e-4f) {
            forward = { 0.0f, 0.0f, -1.0f };
        }
        const Vec3 origin =
            (mode == SceneMode::Play) && playerController.body()
                ? playerController.body()->position()
                : flyCamera.camera.position;
        Vec3 position = origin + glm::normalize(forward) * 3.0f;
        position.y =
            render::terrain::height(terrain.params, position.x, position.z);
        world::ReferenceForm reference;
        reference.id = core::Guid::generate(); // transient (not a record)
        reference.baseForm = base->id;
        reference.position = position;
        world::SpawnContext spawnCtx { world, forms, categories };
        const ecs::Entity entity =
            spawner.spawn(spawnCtx, reference, ecs::Entity {});
        if (!entity.is_alive()) {
            return "'" + args + "' is not a spawnable category";
        }
        streaming.snapCellEntities(makeStreamingContext());
        refreshNpcs(engine->getDevice()); // actors need their rig/brain
        return "spawned " + args + " (transient — not saved)";
    });
    panel.addCommand("tp", [this](const str& args) -> str {
        std::istringstream in { args };
        f32 x = 0.0f, z = 0.0f;
        if (!(in >> x >> z)) {
            return "usage: tp <x> <z>";
        }
        const f32 y = render::terrain::height(terrain.params, x, z) + 0.5f;
        if ((mode == SceneMode::Play) && playerController.body()) {
            playerController.spawnBody(*physics, Vec3 { x, y, z });
        } else {
            flyCamera.camera.position = { x, y + 1.7f, z };
        }
        char out[64];
        std::snprintf(out, sizeof(out), "teleported to %.0f %.0f", x, z);
        return out;
    });
    panel.addCommand("tgm", [this](const str&) -> str {
        return sceneConsole.toggleGodMode() ? "god mode ON" : "god mode OFF";
    });
    panel.addCommand("save", [this](const str& args) -> str {
        saveController.performSave(makeSaveContext(),
                                   args.empty() ? "quick" : args);
        return "saved '" + (args.empty() ? str { "quick" } : args) + "'";
    });
    panel.addCommand("load", [this](const str& args) -> str {
        const str slot = args.empty() ? "quick" : args;
        if (!std::filesystem::exists(savePath(slot))) {
            return "no save named '" + slot + "'";
        }
        saveController.requestLoad(
            slot, [this](const str& m) { interaction.say(m, 3.0f); });
        return "loading '" + slot + "'...";
    });
    panel.addCommand("startquest", [this](const str& args) -> str {
        const auto* quest =
            data::findByEditorId<quest::QuestForm>(forms, args);
        if (!quest) {
            return "no quest named '" + args + "'";
        }
        auto& log = questDirector.questLog();
        if (log.quests.contains(quest->id)) {
            return "'" + args + "' already in the log";
        }
        quest::beginQuest(log, forms, quest->id);
        questDirector.syncQuestTags(makeQuestContext());
        return "quest '" + args + "' started";
    });
    panel.addCommand("queststate", [this](const str&) -> str {
        const auto& log = questDirector.questLog();
        if (log.quests.empty()) {
            return "quest log empty";
        }
        str out;
        for (const auto& [id, progress] : log.quests) {
            const auto* form = forms.find<quest::QuestForm>(id);
            const auto* state =
                forms.find<quest::QuestStateForm>(progress.currentState);
            out += (form ? form->editorId : id.toString()) + ": " +
                   (progress.status == quest::QuestStatus::Succeeded
                        ? "succeeded"
                        : progress.status == quest::QuestStatus::Failed
                              ? "failed"
                              : (state ? state->editorId : "?")) +
                   "  ";
        }
        return out;
    });
    panel.addCommand("settime", [this](const str& args) -> str {
        std::istringstream in { args };
        f32 hour = 0.0f;
        if (!(in >> hour) || hour < 0.0f || hour >= 24.0f) {
            return "usage: settime <0-24>";
        }
        const f64 dayBase =
            std::floor(gameClock.gameDays()) * 86400.0;
        gameClock.gameSeconds =
            dayBase + static_cast<f64>(hour) * 3600.0;
        return "time set";
    });
}

// --- Chantier 4 B4: dialogue (opening + runner in QuestDirector) ---------------------

gameplay::EvalContext LandscapeScene::makeEvalContext() const {
    gameplay::EvalContext context;
    context.tags = &gameTags;
    if (playerEntity.is_alive()) {
        if (playerEntity.has<gameplay::AbilitySystem>()) {
            context.abilitySystem =
                &playerEntity.get<gameplay::AbilitySystem>();
        }
        if (playerEntity.has<gameplay::Inventory>()) {
            context.inventory = &playerEntity.get<gameplay::Inventory>();
        }
    }
    // Lua clauses: no predicate callback wired in the scene yet — such a
    // clause fails closed (the shared VM hookup is a later slice).
    return context;
}

// Bundle the scene systems the quest / crime / dialogue director touches —
// references (the eventBus stays a scene hub) plus the two scene actions it
// needs as closures (the toast, the dialogue model push that needs a
// HudContext). Rebuilt per call (cheap). Mirrors the other make*Context
// builders.
QuestContext LandscapeScene::makeQuestContext() {
    return QuestContext {
        forms,
        gameTags,
        eventBus,
        uiSystem,
        screenStack,
        playerEntity,
        goldForm,
        uiCreated,
        [this](const str& msg, f32 dur) { interaction.say(msg, dur); },
        [this] { hud.pushDialogueModel(makeHudContext()); },
    };
}

// encumbrance gate, and the Crime.Wanted mirror as a closure (it also
// serves the OnPayFine handler). Rebuilt each call (cheap). Mirrors the
// other make*Context builders.
PlayerContext LandscapeScene::makePlayerContext() {
    return PlayerContext {
        forms,
        flyCamera,
        engine->getInput(),
        physics.get(),
        playerEntity,
        gameTags,
        derivedStats,
        statsTuning,
        sprintCostEffect,
        playerWeapon,
        npcDirector.npcs(),
        interaction,
        playerEncumbrance == gameplay::EncumbranceCategory::Overencumbered,
        [this] { questDirector.syncWantedTag(makeQuestContext()); },
    };
}

// --- B6: Forms-driven NPCs (NpcDirector, audit U4-10) -----------------------

// Bundle the NPC subsystem's dependencies for the director this call —
// references into the scene plus the player / weapon / clock scalars and the
// GPU handles it binds. Rebuilt each call (cheap: refs + scalars + handles).
NpcContext LandscapeScene::makeNpcContext() {
    return NpcContext {
        world,
        forms,
        assetDb,
        terrain.params,
        gameTags,
        derivedStats,
        statsTuning,
        eventBus,
        gameClock,
        furnitureOccupancy,
        navigator.get(),
        physics.get(),
        playerEntity,
        playerController.body(),
        mode == SceneMode::Play,
        banditWeapon,
        sceneConsole.godMode(),
        timeSeconds,
        whiteTexture,
        meshSampler,
        *shaders,
        frameBindGroup,
    };
}

// Thin delegators: bundle the context and forward to the director (the many
// call sites — onEnter, streaming ring, travel, console — stay unchanged).
void LandscapeScene::refreshNpcs(rhi::Device& device) {
    npcDirector.refreshNpcs(
        device, makeNpcContext(),
        [this](ecs::Entity entity, const core::Guid& actorFormId) {
            finalizeActorSpawn(entity, actorFormId);
        });
}

void LandscapeScene::updateNpcs(f32 dt) {
    npcDirector.update(dt, makeNpcContext());
}

void LandscapeScene::drawNpcs(engine::FrameContext& frame) {
    npcDirector.draw(frame, makeNpcContext());
}

// Bundle the streaming fixups' systems for StreamingController this frame —
// references into the scene plus the focus / fade / mode scalars. Rebuilt each
// call (cheap: refs + scalars). See StreamingContext (audit U4-10).
StreamingContext LandscapeScene::makeStreamingContext() {
    const Vec3 focus = (mode == SceneMode::Play) && playerController.body()
                           ? playerController.body()->position()
                           : flyCamera.camera.position;
    return StreamingContext {
        world,
        forms,
        terrain.params,
        physics.get(),
        meshCache.get(),
        navigator.get(),
        focus,
        /*fastCook=*/interaction.fading() || interaction.fadeAlpha() > 0.0f,
        /*editorOwnsTransforms=*/mode == SceneMode::Edit,
    };
}

void LandscapeScene::buildShaftPipeline(rhi::Device& device) {
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

void LandscapeScene::drawLightShafts(engine::FrameContext& frame,
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
    world.handle()
        .query<const world::Transform, const world::LightSource>()
        .each([&](flecs::entity e, const world::Transform& transform,
                  const world::LightSource& light) {
            if (!light.shaft) {
                return;
            }
            // Direction: authored (reference rotation x +Z) or the
            // quantized shadow sun (so window shafts follow the day
            // without re-basing every frame).
            Vec3 dir = light.sunLinked
                           ? -shadowSunDirection
                           : transform.rotation * Vec3 { 0.0f, 0.0f, 1.0f };
            if (glm::dot(dir, dir) < 1e-6f) {
                return;
            }
            dir = glm::normalize(dir);
            f32 gate = 1.0f;
            Vec3 color = light.color * light.intensity;
            if (light.sunLinked) {
                gate = glm::smoothstep(0.05f, 0.20f, -dir.y);
                color = sunColor * light.intensity;
            }
            if (interiorMode == false && light.sunLinked) {
                // Exterior sun shafts belong to the volumetric pass.
                return;
            }
            if (gate <= 0.001f) {
                return;
            }

            LightShaft* slot = nullptr;
            for (LightShaft& shaft : lightShafts) {
                if (shaft.entityId == e.id()) {
                    slot = &shaft;
                    break;
                }
            }
            if (!slot) {
                lightShafts.push_back({ e.id() });
                slot = &lightShafts.back();
            }
            slot->seen = true;

            // Rebuild the blades when the direction moves (sun steps).
            if (slot->vertices.id == 0 ||
                glm::dot(slot->cachedDir, dir) < 0.99995f) {
                const f32 length = glm::max(light.shaftLength, 0.5f);
                const f32 halfAngle = glm::radians(
                    glm::clamp(light.spotAngle > 0.0f ? light.spotAngle
                                                      : 30.0f,
                               5.0f, 80.0f) *
                    0.5f);
                const f32 w0 = 0.08f;
                const f32 w1 = std::tan(halfAngle) * length;
                const Vec3 up = std::abs(dir.y) > 0.95f
                                    ? Vec3 { 1.0f, 0.0f, 0.0f }
                                    : Vec3 { 0.0f, 1.0f, 0.0f };
                const Vec3 s0 = glm::normalize(glm::cross(dir, up));
                const Vec3 apex = transform.position;
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
                    const Vec3 side = glm::normalize(
                        glm::angleAxis(angle, dir) * s0);
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
            frame.device.updateBuffer(slot->ubo, uniforms,
                                      sizeof(uniforms), 0);
            if (!any) {
                frame.cmd.setPipeline(shaftPipeline);
                frame.cmd.setBindGroup(0, frameBindGroup);
                any = true;
            }
            frame.cmd.setBindGroup(1, slot->group);
            frame.cmd.setVertexBuffer(0, slot->vertices);
            frame.cmd.draw(slot->vertexCount);
        });
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

f32 LandscapeScene::effectiveWaterSurfaceY() const {
    // Brick 32: the water surface the CAMERA sits under, if any — sea
    // level outdoors, a volume's top when inside one (any worldspace),
    // "dry" otherwise. Feeds the tonemap submersion.
    f32 surface = interiorMode ? -1.0e6f : terrain.params.seaLevel;
    const Vec3 eye = flyCamera.camera.position;
    world.handle()
        .query<const world::Transform, const world::WaterVolume>()
        .each([&](flecs::entity, const world::Transform& transform,
                  const world::WaterVolume& volume) {
            const Vec3 d = eye - transform.position;
            if (std::abs(d.x) <= volume.halfExtents.x &&
                std::abs(d.z) <= volume.halfExtents.z && d.y >= 0.0f &&
                d.y <= volume.halfExtents.y * 2.0f) {
                surface = glm::max(
                    surface,
                    transform.position.y + volume.halfExtents.y * 2.0f);
            }
        });
    return surface;
}

void LandscapeScene::drawWaterVolumes(engine::FrameContext& frame) {
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
    world.handle()
        .query<const world::Transform, const world::WaterVolume>()
        .each([&](flecs::entity e, const world::Transform& transform,
                  const world::WaterVolume& volume) {
            WaterQuad* slot = nullptr;
            for (WaterQuad& quad : waterQuads) {
                if (quad.entityId == e.id()) {
                    slot = &quad;
                    break;
                }
            }
            if (!slot) {
                waterQuads.push_back({ e.id() });
                slot = &waterQuads.back();
            }
            slot->seen = true;
            if (slot->vertices.id == 0) {
                // The box TOP face, two triangles in world space.
                const Vec3 c = transform.position +
                               Vec3 { 0.0f, volume.halfExtents.y * 2.0f,
                                      0.0f };
                const f32 hx = volume.halfExtents.x;
                const f32 hz = volume.halfExtents.z;
                const f32 verts[18] = {
                    c.x - hx, c.y, c.z - hz, c.x + hx, c.y, c.z - hz,
                    c.x + hx, c.y, c.z + hz, c.x - hx, c.y, c.z - hz,
                    c.x + hx, c.y, c.z + hz, c.x - hx, c.y, c.z + hz,
                };
                slot->vertices = frame.device.createBuffer(
                    { .usage = rhi::BufferUsage::Vertex,
                      .size = sizeof(verts) },
                    verts);
                slot->ubo = frame.device.createBuffer(
                    { .usage = rhi::BufferUsage::Uniform,
                      .size = sizeof(Vec4),
                      .dynamic = true },
                    nullptr);
                slot->group = frame.device.createBindGroup(
                    { .entries = { { .binding = 1,
                                     .buffer = slot->ubo } } });
                const Vec4 tint { volume.tint, volume.chop };
                frame.device.updateBuffer(slot->ubo, &tint, sizeof(tint),
                                          0);
            }
            if (!any) {
                frame.cmd.setPipeline(waterVolumePipeline);
                frame.cmd.setBindGroup(0, frameBindGroup);
                any = true;
            }
            frame.cmd.setBindGroup(1, slot->group);
            frame.cmd.setVertexBuffer(0, slot->vertices);
            frame.cmd.draw(6);
        });
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

void LandscapeScene::buildCasterPipelines(rhi::Device& device) {
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

void LandscapeScene::drawShadowCasters(engine::FrameContext& frame,
                                       u32 cascade) {
    drawCastersInto(frame, shadows.casterBindGroup(cascade), cascade == 0);
}

void LandscapeScene::drawCastersInto(engine::FrameContext& frame,
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
            const MeshCache::Gpu& mesh = meshCache->resolve(instance.model);
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
    // 2048px cascade resolution.
    if (!npcDirector.npcs().empty()) {
        frame.cmd.setPipeline(skinnedCasterPipeline);
        frame.cmd.setBindGroup(1, casterGroup);
        for (auto& npcPtr : npcDirector.npcs()) {
            Npc& npc = *npcPtr;
            if (npc.modelUbo.id == 0 || !npc.entity.is_alive()) {
                continue;
            }
            if (npc.casterGroup.id == 0) {
                npc.casterGroup = frame.device.createBindGroup(
                    { .entries = { { .binding = 4, .buffer = npc.modelUbo },
                                   { .binding = 2,
                                     .buffer = npc.paletteSsbo,
                                     .storage = true } } });
            }
            frame.cmd.setBindGroup(2, npc.casterGroup);
            frame.cmd.setVertexBuffer(0, npc.vertices);
            frame.cmd.setIndexBuffer(npc.indices, rhi::IndexFormat::U32);
            frame.cmd.drawIndexed(npc.indexCount);
        }
    }
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
    if (!interiorMode) { // interiors: no terrain/scatter/water to stream
        {
            core::FrameProbe::Scope probe { frameProbe, "terrain" };
            terrain.update(frame.device, flyCamera.camera.position);
            // 33b/c: pump/kick the light-map bake (worker; re-bakes on
            // the quantized sun step or when the focus strays).
            terrainLightMap.update(frame.device, terrain.params,
                                   flyCamera.camera.position,
                                   shadowSunDirection);
        }
        // Height-horizon occlusion (brick 26): rebuilt on a worker
        // whenever the camera strays; stays valid (conservative) meanwhile.
        {
            core::FrameProbe::Scope probe { frameProbe, "occlusion" };
            occlusion.pump();
            if (occlusion.wantsRebuild(flyCamera.camera.position)) {
                occlusion.rebuild(terrain.params, flyCamera.camera.position,
                                  terrain.chunkTops());
            }
        }
        {
            core::FrameProbe::Scope probe { frameProbe, "grass" };
            grass.update(frame.device, terrain.params,
                         flyCamera.camera.position);
        }
        {
            core::FrameProbe::Scope probe { frameProbe, "veg" };
            vegetation.update(frame.device, terrain.params,
                              flyCamera.camera.position);
        }
        if (frame.device.caps().copyTexture) {
            core::FrameProbe::Scope probe { frameProbe, "water" };
            water.update(frame.device, terrain.params,
                         flyCamera.camera.position);
        }
    }

    const render::Camera3D& camera = flyCamera.camera;
    const Mat4 viewProj = camera.viewProj(frame.aspect);
    // CPU chunk culling (brick 25): one frustum per rendered viewpoint.
    const render::Frustum viewFrustum = render::Frustum::fromViewProj(viewProj);
    const render::SkySystem::SkyState skyState =
        sky.evaluate({ .cloudCoverage = atmos.cloudCoverage,
                       .sunIntensity = atmos.sunIntensity,
                       .ambientIntensity = atmos.ambientIntensity,
                       .saturation = atmos.saturation,
                       .warmth = atmos.warmth });

    // Shadows ramp out as the sun crosses the horizon (no sun, no shadows),
    // and soften away under heavy cloud cover (diffuse light casts none).
    const bool shadowsAvailable = shadows.receiverBindGroup().id != 0;
    const f32 shadowStrength =
        (shadowsUi && shadowsAvailable && !interiorMode)
            ? glm::smoothstep(-0.02f, 0.06f, skyState.sunDirection.y) *
                  (1.0f - 0.65f * atmos.cloudCoverage)
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
        reflectionsUi && reflectionFb.id != 0 && !interiorMode &&
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
        .time = { timeSeconds, ssaoUi, atmos.volumetric,
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
                      cascadeDebugUi ? 1.0f : 0.0f, atmos.bloomIntensity },
        .fogInfo = { atmos.fogDensity, atmos.fogHeightFalloff, atmos.fogLowBoost,
                     atmos.fogStart },
        .sunViewProj = cascades.viewProj,
        // .w = interior flag (B5): mesh/skinned/locallights switch to the
        // hemispheric ambient + wrap/bounce indoors; 0 keeps the exterior
        // byte-identical.
        .cascadeSplits = { cascades.splitFar[0], cascades.splitFar[1],
                           cascades.splitFar[2],
                           interiorMode ? 1.0f : 0.0f },
        .shadowInfo = { cascades.texelWorld[0], cascades.texelWorld[1],
                        cascades.texelWorld[2], shadowStrength },
        .screenInfo = { static_cast<f32>(frame.width),
                        static_cast<f32>(frame.height),
                        1.0f / static_cast<f32>(frame.width),
                        1.0f / static_cast<f32>(frame.height) },
        .cloudInfo = { atmos.cloudCoverage, atmos.cloudHeight, atmos.cloudScale,
                       atmos.cloudShadow },
        .sunScreen = { sunUv.x, sunUv.y, shaftFade, atmos.godRayIntensity },
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
        .windInfo = { windTime, atmos.windStrength, atmos.waveChop, 0.0f },
    };
    render::FrameUniforms frameData = uniforms;
    if (interiorMode) {
        // B7 interior mode: no sun, no sky glow, dim constant ambient, no
        // fog, no god rays/volumetric — local lights (B5) carry the room.
        frameData.sunColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        frameData.sunGlowColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        frameData.ambientColor = { tuning.interiorAmbient,
                                   uniforms.ambientColor.w };
        frameData.fogInfo = { 0.0f, 0.02f, 0.0f, 100000.0f };
        frameData.sunScreen = { 0.5f, 0.5f, 0.0f, 0.0f };
        frameData.time.z = 0.0f; // volumetric shafts off
    }
    // B3 (brick 28): grade parameters on free .w slots — AFTER the
    // interior override (which zeroes sunGlowColor), so the grade applies
    // in both modes. Neutral values when the A/B toggle is off.
    frameData.sunGlowColor.w = gradingUi ? gradeVibranceUi : 0.0f;
    frameData.zenithColor.w = gradingUi ? gradeSplitToneUi : 0.0f;
    frameData.horizonColor.w = gradingUi ? gradeContrastUi : 1.0f;
    // Brick 32: the effective water surface above the camera (sea /
    // volume top / dry) — the tonemap submersion input.
    frameData.submersionInfo.x = effectiveWaterSurfaceY();
    // 33b/c: the terrain light map info (w = strength, 0 until the first
    // bake lands or when toggled off / indoors).
    frameData.terrainLightInfo = terrainLightMap.info();
    frameData.terrainLightInfo.w =
        (terrainLightUi && !interiorMode && terrainLightMap.ready()) ? 1.0f
                                                                     : 0.0f;
    // 7.8ter: the player's feet part the grass (off in Fly).
    const phys::CharacterBody* playerBody = playerController.body();
    frameData.grassBendInfo =
        (mode == SceneMode::Play) && playerBody
            ? Vec4 { playerBody->position().x, playerBody->position().z,
                     playerBody->position().y, 0.85f }
            : Vec4 { 0.0f };
    // Brick 30/31: the crossfaded storm front + rain intensity, and the
    // top-down rain-occlusion matrix (ortho, 40 m around the camera).
    frameData.stormInfo.x = atmos.stormFront;
    frameData.stormInfo.y = interiorMode ? 0.0f : atmos.rainIntensity;
    if (frameData.stormInfo.y > 0.003f) {
        const Vec3 eye = camera.position;
        const Mat4 rainView =
            glm::lookAt(eye + Vec3 { 0.0f, 60.0f, 0.0f }, eye,
                        Vec3 { 0.0f, 0.0f, 1.0f });
        const Mat4 rainProj =
            glm::ortho(-40.0f, 40.0f, -40.0f, 40.0f, 0.0f, 140.0f);
        frameData.rainOcclusionViewProj = rainProj * rainView;
        frame.device.updateBuffer(rainOcclusionUbo,
                                  &frameData.rainOcclusionViewProj,
                                  sizeof(Mat4), 0);
    }
    // B4 (brick 29): auto-exposure parameters on free .w slots (adapt.frag
    // + the tonemap tap flag).
    frameData.sunDirection.w = frame.dt;
    frameData.horizonFarColor.w = autoExposureMinUi;
    frameData.cloudMapInfo.w = autoExposureMaxUi;
    frameData.windInfo.w = autoExposureUi ? 1.0f : 0.0f;
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
        const vector<SceneLight> nearest =
            collectLights(world, camera.position, kMaxLights);
        lights.count.x = static_cast<f32>(nearest.size());
        for (u32 i = 0; i < nearest.size(); ++i) {
            const SceneLight& light = nearest[i];
            f32 intensity = light.intensity;
            if (light.flicker > 0.0f) {
                const f32 phase = static_cast<f32>(i) * 1.7f;
                intensity *=
                    1.0f + light.flicker *
                               (0.55f * std::sin(timeSeconds * 9.0f + phase) +
                                0.45f * std::sin(timeSeconds * 23.0f +
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
    if (!interiorMode) {
        sky.bakeCloudMap(frame.cmd, frameBindGroup);
    }

    // B2b — the interior key-light shadow: pick the castsShadow light
    // nearest the camera, render its perspective depth, and hand the
    // matrix + position to locallights.glsl (matched by position there).
    bool keyShadowActive = false;
    if (keyShadowUi && interiorMode && meshShadowCastersUi) {
        f32 bestDistSq = 1e12f;
        Vec3 keyPos {};
        Vec3 keyDir { 0.0f, 0.0f, 1.0f };
        f32 keyFov = 100.0f;
        f32 keyRadius = 10.0f;
        world.handle()
            .query<const world::Transform, const world::LightSource>()
            .each([&](flecs::entity, const world::Transform& transform,
                      const world::LightSource& light) {
                if (!light.castsShadow) {
                    return;
                }
                const Vec3 d = transform.position - camera.position;
                const f32 distSq = glm::dot(d, d);
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    keyPos = transform.position;
                    keyDir = transform.rotation * Vec3 { 0.0f, 0.0f, 1.0f };
                    keyFov = light.spotAngle > 0.0f
                                 ? glm::min(light.spotAngle * 1.3f, 150.0f)
                                 : 120.0f;
                    keyRadius = light.radius;
                }
            });
        if (bestDistSq < 1e12f) {
            const Vec3 up = std::abs(keyDir.y) > 0.95f
                                ? Vec3 { 1.0f, 0.0f, 0.0f }
                                : Vec3 { 0.0f, 1.0f, 0.0f };
            const Mat4 view = glm::lookAt(keyPos, keyPos + keyDir, up);
            const Mat4 proj = glm::perspective(
                glm::radians(keyFov), 1.0f, 0.05f, keyRadius);
            frameData.keyShadowViewProj = proj * view;
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
            drawCastersInto(frame, keyShadowCasterGroup,
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
        drawCastersInto(frame, rainCasterGroup, /*refreshUbos=*/true);
        frame.cmd.endRenderPass();
    }

    // Cascade passes: depth-only casters from the sun's point of view.
    if (shadowStrength > 0.0f) {
        core::FrameProbe::Scope probe { frameProbe, "shadows" };
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
                drawShadowCasters(frame, i);
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

        core::FrameProbe::Scope reflectionProbe { frameProbe, "reflection" };
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
        core::FrameProbe::Scope probe { frameProbe, "mainPass" };
        frame.cmd.beginRenderPass(
            { .framebuffer =
                  useOffscreen ? offscreenFb : rhi::FramebufferHandle {},
              .loadOp =
                  interiorMode ? rhi::LoadOp::Clear : rhi::LoadOp::DontCare,
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
        if (!interiorMode) {
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
        drawSceneMeshes(frame); // B1: the RenderSnapshot.meshes consumer
        drawNpcs(frame);        // B6: the Forms-driven skinned NPCs
        if (!interiorMode) {
            sky.draw(frame.cmd, frameBindGroup); // background only
        }
        // Brick 30: horizon cumulonimbus, right after the sky dome (they
        // occlude sky, terrain occludes them via the depth test).
        if (!interiorMode && atmos.stormFront > 0.003f) {
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
        drawWaterVolumes(frame);
        drawLightShafts(frame, skyState.sunColor);
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
        core::FrameProbe::Scope probe { frameProbe, "copyHizWater" };
        frame.cmd.copyTexture(offscreenColor, sceneColorCopy);
        frame.cmd.copyTexture(offscreenDepth, sceneDepthCopy);

        // GPU Hi-Z occlusion (brick 26): pyramid from this frame's depth
        // snapshot + cull dispatch; the verdict is read back NEXT frame.
        if (!interiorMode && frame.device.caps().computeShaders) {
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

        if (!interiorMode) {
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
        core::FrameProbe::Scope probe { frameProbe, "postfx" };
        if (sky.cloudMapBindGroup().id != 0) {
            frame.cmd.setBindGroup(3, sky.cloudMapBindGroup());
        }
        postFx.render(frame.cmd, frameBindGroup,
                      shadows.receiverBindGroup());
        // 33a: contact shadows (the texture is the toggle — white = off).
        if (contactShadowsUi && !interiorMode) {
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
        core::FrameProbe::Scope probe { frameProbe, "composite" };
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
        if (uiCreated) {
            uiSystem.resize(frame.width, frame.height); // no-op if unchanged
            uiSystem.render(frame.cmd, frame.device, frame.width,
                            frame.height);
        }
        frame.cmd.endRenderPass();
    } else if (uiCreated) {
        frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Load,
                                    .depthLoadOp = rhi::LoadOp::DontCare });
        uiSystem.resize(frame.width, frame.height);
        uiSystem.render(frame.cmd, frame.device, frame.width, frame.height);
        frame.cmd.endRenderPass();
    }
    // Stutter hunt: one WARN line with the block breakdown on any frame
    // > 25 ms. If `probed` sits far below the total, the spike lives
    // outside the instrumented blocks (present/driver/OS).
    frameProbe.endFrame();
}

void LandscapeScene::drawUi() {
    // The travel fade stays an ImGui overlay (a plain black quad, not a
    // moddable screen). Prompt + talk line moved to the RmlUi HUD
    // (chantier 4 B2); the ImGui fallback below only serves when the game
    // UI failed to create.
    ImDrawList* foreground = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!uiCreated) {
        if ((mode == SceneMode::Play) && interaction.promptVisible()) {
            const str& prompt = interaction.promptLabel();
            const ImVec2 size = ImGui::CalcTextSize(prompt.c_str());
            foreground->AddText(
                { (display.x - size.x) * 0.5f, display.y * 0.62f },
                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.95f)),
                prompt.c_str());
        }
        if (interaction.talkVisible()) {
            const str& talk = interaction.talkLine();
            const ImVec2 size = ImGui::CalcTextSize(talk.c_str());
            foreground->AddText(
                { (display.x - size.x) * 0.5f, display.y * 0.55f },
                ImGui::GetColorU32(ImVec4(1.0f, 0.95f, 0.8f, 0.95f)),
                talk.c_str());
        }
    }
    if (interaction.fadeAlpha() > 0.0f) {
        foreground->AddRectFilled(
            { 0.0f, 0.0f }, display,
            ImGui::GetColorU32(
                ImVec4(0.0f, 0.0f, 0.0f, interaction.fadeAlpha())));
    }

    // Mode hotkeys. Play is home: F2 toggles Spectator (a paused free camera —
    // the photo-mode base), F3 toggles the level editor. Each key returns to
    // Play when pressed again. Both are inert while a modal menu owns the input
    // (grabbing the mouse for Play would fight the menu). F2/F3 (not F11/F12)
    // stay clear of the CLion/VS debugger's global Step Into/Over shortcuts.
    if (!screenStack.modalOpen()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            if (mode == SceneMode::Spectator) {
                enterPlayMode(); // back to Play (home)
            } else {
                if (mode == SceneMode::Edit) {
                    sceneEditor.deselect();
                }
                if (mode == SceneMode::Play) {
                    exitPlayMode(); // -> Spectator (tears down the capsule)
                } else {
                    mode = SceneMode::Spectator; // from Edit
                }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false) && levelEditor) {
            if (mode == SceneMode::Edit) {
                sceneEditor.deselect();
                enterPlayMode(); // editor off -> back to Play (home)
            } else {
                if (mode == SceneMode::Play) {
                    exitPlayMode(); // tears down the capsule (the editor flies)
                }
                mode = SceneMode::Edit;
            }
        }
    }
    if (mode == SceneMode::Edit && levelEditor) {
        sceneEditor.draw(makeEditorContext());
    }

    // Chantier 5 B5: quicksave / quickload.
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        saveController.performSave(makeSaveContext(), "quick");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        saveController.requestLoad(
            "quick", [this](const str& m) { interaction.say(m, 3.0f); });
    }

    // F8 toggles the dev console (chantier 4 B7 — spawn/tp/tgm/settime,
    // reflection get/set, Lua).
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) {
        sceneConsole.toggle(mode == SceneMode::Play);
        if (mode == SceneMode::Play) {
            // Free the cursor while the console is open (typing needs it and
            // mouselook must stop); recapture on close unless a modal still
            // owns the mouse.
            const bool capture =
                !sceneConsole.visible() && !screenStack.modalOpen();
            engine->getWindow().setRelativeMouseMode(capture);
        }
    }
    sceneConsole.draw();

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
    if ((mode == SceneMode::Play)) {
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
    // The clock is the source of truth: the slider WRITES it (day count
    // preserved), the sky follows in update().
    f32 hour = static_cast<f32>(std::fmod(gameClock.gameHours(), 24.0));
    if (ImGui::SliderFloat("Time of day (h)", &hour, 0.0f, 24.0f, "%.1f")) {
        const f64 days = std::floor(gameClock.gameHours() / 24.0);
        gameClock.gameSeconds = (days * 24.0 + hour) * 3600.0;
    }
    ImGui::Checkbox("Animate (24 h in 2 min)", &animateTime);
    ImGui::SameLine();
    ImGui::TextDisabled("day %d, x%.0f", static_cast<int>(gameClock.gameDays()),
                        gameClock.timescale);
    if (!weather.states().empty()) {
        // "(manual)" entry + one per WeatherForm, separated by '\0' as
        // ImGui::Combo expects (c_str() supplies the double terminator).
        str items = "(manual)";
        items.push_back('\0');
        for (const WeatherForm& w : weather.states()) {
            items += w.editorId;
            items.push_back('\0');
        }
        int selected = weather.selected() + 1;
        if (ImGui::Combo("Weather", &selected, items.c_str())) {
            // Depart from whatever is on screen right now — mid-fade switches
            // stay continuous.
            weather.beginTransition(selected - 1, atmos);
        }
        ImGui::SliderFloat("Transition (s)", &weather.duration(), 1.0f, 120.0f,
                           "%.0f", ImGuiSliderFlags_Logarithmic);
        if (weather.transitioning()) {
            ImGui::SameLine();
            ImGui::Text("%.0f%%", weather.blend() * 100.0f);
        }
        ImGui::SliderFloat("Wind strength", &atmos.windStrength, 0.0f, 2.5f,
                           "%.2f");
        ImGui::SliderFloat("Wave chop", &atmos.waveChop, 0.0f, 2.5f, "%.2f");
        ImGui::SliderFloat("Sun intensity", &atmos.sunIntensity, 0.0f, 1.5f,
                           "%.2f");
        ImGui::SliderFloat("Ambient intensity", &atmos.ambientIntensity, 0.0f,
                           1.5f, "%.2f");
        ImGui::SliderFloat("Saturation", &atmos.saturation, 0.0f, 1.3f, "%.2f");
        ImGui::SliderFloat("Warmth (dawn/dusk)", &atmos.warmth, 0.0f, 1.0f,
                           "%.2f");
    }
}

void LandscapeScene::drawGameplayUi() {
    if (!npcDirector.npcs().empty()) {
        // B6: the Forms-driven NPC — patrol state + locomotion graph live.
        static constexpr const char* kStateNames[] = { "idle", "walk",
                                                       "run" };
        const Npc& npc = *npcDirector.npcs().front();
        const u32 state = npc.anim->currentState();
        ImGui::Text("NPC: %s%s | %.1f m/s",
                    state < 3 ? kStateNames[state] : "sit/other",
                    npc.anim->blending() ? " (blending)" : "", npc.speed);
        if (!npc.intentReason.empty()) {
            // The P0 schedule tool: where is this NPC going, and why.
            ImGui::TextDisabled("intent: %s", npc.intentReason.c_str());
        }
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
        bool play = (mode == SceneMode::Play);
        if (ImGui::Checkbox("Play mode — F2 Spectator / F3 Editor", &play)) {
            play ? enterPlayMode() : exitPlayMode();
        }
        if ((mode == SceneMode::Play)) {
            ImGui::TextUnformatted("WASD: move | Shift: sprint | Space: jump | "
                                   "F2: Spectator | F3: Editor");
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
                glm::length(playerController.smoothedVelocity()));
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
        ImGui::Text("Collision tiles: %u | cells loaded: %u",
                    terrainCollision->tileCount(),
                    cellStreamer ? cellStreamer->loadedCount() : 0);
    }
}

void LandscapeScene::drawRenderUi() {
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
