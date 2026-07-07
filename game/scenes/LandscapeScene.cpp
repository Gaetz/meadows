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

// B5.5: stat-space -> world mapping (docs/STATS.md §3; the CombatArena's
// kSpeedScale precedent, recalibrated for meters). Default sheet (~102):
// jog ~5.1 m/s, sprint x1.6 ~8.2 m/s, velocity settles in ~0.1 s.
// (Dev feel pass 2026-07-06: +50% — the unencumbered adventurer is brisk;
// encumbrance will pull it back down when the P1 utility pass lands.)
constexpr f32 kSpeedScale3D = 1.0f / 20.0f; // movementSpeed stat -> m/s
constexpr f32 kSprintMult = 1.6f;           // "sprint multiplies" (STATS.md)
constexpr f32 kJumpScale3D = 1.0f / 20.8f;  // jumpPower stat -> jump m/s
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
    // layer — one more plugin, the §5 invariant in action.
    std::optional<data::Plugin> savePlugin;
    loadedFromSave = false;
    if (!pendingLoadSlot.empty()) {
        savePlugin = readSave(pendingLoadSlot, formTypes);
        if (savePlugin) {
            LOG_INFO("Loading save '{}' ({} records)", pendingLoadSlot,
                     savePlugin->records.size());
            loadedFromSave = true;
        } else {
            LOG_WARN("save '{}' not found", pendingLoadSlot);
        }
        pendingLoadSlot.clear();
    }
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
    weathers = resolveWeatherForms(forms);
    LOG_INFO("Landscape tuning: seed={} seaLevel={} fogDensity={} "
             "coverage={} | {} weather states",
             tuning.terrainSeed, tuning.seaLevel, tuning.fogDensity,
             tuning.cloudCoverage, weathers.size());

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
    gradeVibranceUi = tuning.gradeVibrance;   // B3 (toggle stays off)
    gradeSplitToneUi = tuning.gradeSplitTone;
    gradeContrastUi = tuning.gradeContrast;
    autoExposureMinUi = tuning.autoExposureMin; // B4 (toggle stays off)
    autoExposureMaxUi = tuning.autoExposureMax;

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
    gameTags.registerTag("State.Dead");
    gameTags.registerTag("State.Staggered");
    gameTags.registerTag("State.Paralyzed");
    gameTags.registerTag("State.Exhausted");
    gameTags.registerTag("State.Shaken");
    gameTags.registerTag("State.CriticalWeakness");
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
    // Chantier 3 B6: the melee weapons (data — retune in village.toml).
    playerWeapon =
        data::findByEditorId<data::WeaponForm>(forms, "RustySword");
    banditWeapon =
        data::findByEditorId<data::WeaponForm>(forms, "BanditClub");
    // Chantier 4 B5: the currency + the barter trigger (a dialogue node
    // fires "OpenBarter" — the vendor is whoever we're talking to).
    goldForm = data::findByEditorId<data::MiscItemForm>(forms, "GoldCoin");
    eventBus = gameplay::EventBus {};
    eventBus.subscribe(gameplay::eventKind("OpenBarter"),
                       [this](const gameplay::Event&) {
                           openBarterScreen(dialoguePartner);
                       });
    // Chantier 6 A2: the quest wiring. Gate tags registered up front so
    // dialogue conditions evaluate before any quest starts.
    questLog = quest::QuestLog {};
    easternQuest =
        data::findByEditorId<quest::QuestForm>(forms, "EasternMenace");
    for (const char* tag :
         { "Quest.EasternMenace.Active", "Quest.EasternMenace.Ready",
           "Quest.EasternMenace.Done", "Crime.Wanted" }) {
        gameTags.registerTag(tag);
    }
    eventBus.subscribe(
        gameplay::eventKind("OnAcceptEasternMenace"),
        [this](const gameplay::Event&) {
            if (!easternQuest ||
                questLog.quests.contains(easternQuest->id)) {
                return; // already taken (or done) — never re-begin
            }
            quest::beginQuest(questLog, forms, easternQuest->id);
            syncQuestTags();
            talkLine = "Nouvelle quete : La menace de l'est (journal : J).";
            talkTimer = 4.0f;
        });
    eventBus.subscribe(gameplay::eventKind("OnDeath"),
                       [this](const gameplay::Event& event) {
                           handleQuestEvent(event);
                       });
    eventBus.subscribe(gameplay::eventKind("OnReportBandit"),
                       [this](const gameplay::Event& event) {
                           handleQuestEvent(event);
                       });
    // Chantier 6 D2: paying the fine clears the bounty. The option is
    // gated by HasTag Crime.Wanted + HasItem gold ≥ 40 in data, so the
    // removeItem below can only fail if a mod broke the gate — then it
    // simply does nothing.
    eventBus.subscribe(
        gameplay::eventKind("OnPayFine"), [this](const gameplay::Event&) {
            if (!playerEntity.is_alive() || !goldForm) {
                return;
            }
            auto& bag = playerEntity.get_mut<gameplay::Inventory>();
            if (!gameplay::removeItem(bag, goldForm->id, 40)) {
                return;
            }
            if (playerEntity.has<gameplay::Bounty>()) {
                playerEntity.get_mut<gameplay::Bounty>().bounty = 0.0f;
            }
            syncWantedTag();
            talkLine = "Amende payee. Restez dans le droit chemin.";
            talkTimer = 4.0f;
        });
    // Chantier 6 A4: a loaded save rebuilds the quest log (the tags
    // mirror re-syncs after the player spawns, below).
    if (loadedFromSave) {
        quest::applySavedQuests(questLog, forms);
    }

    world = ecs::World {}; // fresh on re-enter
    world::registerSceneComponents(world);
    gameplay::registerGameplayComponents(world);
    // Per-frame queries, built once against the fresh world.
    doorQuery = world.handle()
                    .query<const world::Transform, const world::DoorTarget>();
    interactQuery =
        world.handle().query<const world::Transform, const world::RefId>();
    colliderQuery =
        world.handle()
            .query<const world::Transform, const world::RefId,
                   const world::MeshRender>();
    nonCollidable.clear();
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
    pendingSave.clear();
    cellLoader->beforeUnload = [this](data::FormHandle,
                                      ecs::Entity cellEntity) {
        pendingSave.captureCell(world, forms, cellEntity, gameTags);
    };
    cellLoader->spawnFilter = [this](const core::Guid& referenceId) {
        return pendingSave.isEnabled(referenceId);
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
    fadeAlpha = 0.0f;
    fadeDirection = 0;
    pendingTravel = core::Guid {};
    // Chantier 3 B1: start the day at 10:00, ~7.5 real minutes per game
    // hour (timescale 12 — "Animate" boosts it).
    gameClock = gameplay::GameClock {};
    gameClock.gameSeconds = 10.0 * 3600.0;
    gameClock.timescale = 12.0f;
    // Chantier 5 B5: the WorldStateForm of a loaded save overrides the
    // fresh-game defaults (clock, worldspace; the camera is restored at
    // the end of onEnter, after the start-spot heuristic).
    loadedWorldState.reset();
    if (loadedFromSave) {
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
    editMode = false;
    editSelection = ecs::Entity {};
    placementBase = core::Guid {};
    createConsole(); // chantier 4 B7: F8 in-game dev console

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
        syncQuestTags(); // A4: re-mirror a loaded quest log onto the player
        syncWantedTag(); // D2: re-mirror a loaded bounty (tag not persisted)
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
    buildSkinnedPipeline(device);

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
    snapCellEntities();
    refreshNpcs(device);
    refreshNavObstacles();

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

    // Chantier 5 B5: a loaded game resumes where it stood — camera on the
    // player, saved look angles, straight into Play (no boot menu). The
    // capsule spawns at the SAVED position directly (the travel pattern —
    // enterPlayMode would re-ground on the terrain, wrong indoors).
    if (loadedFromSave) {
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
            player = std::make_unique<phys::CharacterBody>(
                *physics, 0.3f, 1.8f, feet + Vec3 { 0.0f, 0.25f, 0.0f });
            playerVelocity = Vec3 { 0.0f };
            playMode = true;
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
    if (kDevStartInterior && !loadedFromSave) {
        // HouseDoorExterior's arrival marker (village.toml, the marker
        // REFERENCE inside the interior cell).
        if (const auto marker = core::Guid::fromString(
                "4d7a9b30-0000-4000-8000-000000000023");
            marker && forms.find<world::ReferenceForm>(*marker)) {
            pendingTravel = *marker;
            fadeDirection = 1;
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
    dialogueRunner.reset(); // references `forms`, reset before re-resolve
    dialogueOptions.clear();
    containerEntity = ecs::Entity {};
    dialoguePartner = ecs::Entity {};
    barterMode = false;
    goldForm = nullptr;
    easternQuest = nullptr; // points into `forms`
    questLog = quest::QuestLog {};
    console.reset(); // references forms/session — before re-resolve
    consoleVm.reset();
    consoleSession.reset();
    consoleVisible = false;
    godMode = false;
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
    // B6 NPCs: GPU state per NPC, then the CPU-side rig cache.
    for (auto& npc : npcs) {
        destroyNpc(device, *npc);
    }
    npcs.clear();
    patrolPoints.clear();
    rigCache.clear();
    device.destroyPipeline(skinnedPipeline);
    skinnedPipeline = {};
    // Chantier 2 B1: cell machinery (references scene members — release
    // before the members are reset on the next onEnter).
    cellStreamer.reset();
    cellLoader.reset();
    overworldHandle = data::FormHandle {};
    // B4/B5 physics: bodies -> tiles -> world (each references the previous).
    playMode = false;
    player.reset();
    debugCapsule.reset();
    for (const auto& [entity, body] : staticColliders) {
        physics->removeBody(body);
    }
    staticColliders.clear();
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
    // B4/B5: physics tick + collision tiles around the focus (the player
    // in Play mode, the camera in Fly); the debug capsule free-falls.
    if (physics && !uiPaused) {
        core::FrameProbe::Scope probe { frameProbe, "physics" };
        physics->tick(dt);
        if (!interiorMode) { // interiors have no terrain to collide with
            const Vec3 focus = playMode && player
                                   ? player->position()
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
        const Vec3 focus = playMode && player ? player->position()
                                              : flyCamera.camera.position;
        // Chantier 5 B8: border crossings spread their spawns — one cell
        // per frame (the initial ring and travels load whole, behind the
        // fade). Fixups are idempotent, re-run per loaded cell.
        if (cellStreamer->update(activeWorldspace, focus.x, focus.z, 2, 3,
                                 /*maxLoads=*/1)) {
            snapCellEntities();
            refreshNpcs(engine->getDevice());
            refreshNavObstacles();
        }
    }
    {
        // B2: bodies follow spawns + mesh residency.
        core::FrameProbe::Scope probe { frameProbe, "colliders" };
        updateStaticColliders();
    }
    // The sky follows the clock UNCONDITIONALLY — it is presentation, not
    // sim. Gating it on !uiPaused made the wait menu look broken: +8 h
    // from the pause chain landed in gameSeconds, but the world behind
    // the (still open) pause menu kept the stale sun.
    sky.timeOfDay =
        static_cast<f32>(std::fmod(gameClock.gameHours(), 24.0));
    if (!uiPaused) {
        // B7/ch.3 B1: interaction prompts + the travel fade state machine.
        updateInteraction(dt);
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
    if (!uiPaused) {
        core::FrameProbe::Scope probe { frameProbe, "npcs" };
        updateNpcs(dt);
    }
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
        blended.stormFront = lerp(weatherFrom.stormFront,
                                  to.stormFront); // brick 30
        blended.rainIntensity = lerp(weatherFrom.rainIntensity,
                                     to.rainIntensity); // brick 31
        applyWeather(blended);
    }

    // B5: F toggles first-person Play mode (unless ImGui owns the
    // keyboard). In Play the player drives; Fly stays the dev camera.
    if (!uiPaused && engine->getInput().wasPressed(platform::Key::F) &&
        !ImGui::GetIO().WantCaptureKeyboard) {
        playMode ? exitPlayMode() : enterPlayMode();
    }
    if (uiPaused) {
        // A modal screen owns the input; cameras and player hold still.
    } else if (playMode && player) {
        updatePlayer(dt);
    } else {
        // Don't steal the mouse from ImGui: clicking a panel must not
        // mouselook.
        const bool allowCapture = !ImGui::GetIO().WantCaptureMouse;
        flyCamera.update(engine->getInput(), engine->getWindow(), dt,
                         allowCapture);
    }
    // (Time-of-day now advances through the game clock, above.)

    // Chantier 5 B5: a requested load re-enters the scene with the save
    // resolved as the last layer. End of update: nothing touches the
    // world after this.
    if (reloadRequested) {
        reloadRequested = false;
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
    player =
        std::make_unique<phys::CharacterBody>(*physics, 0.3f, 1.8f, feet);
    playerVelocity = Vec3 { 0.0f };
    playMode = true;
    engine->getWindow().setRelativeMouseMode(true);
    screenStack.show("hud"); // the HUD overlay lives with Play mode
}

void LandscapeScene::exitPlayMode() {
    playMode = false;
    player.reset();
    engine->getWindow().setRelativeMouseMode(false);
    screenStack.close("hud");
    // The camera stays where the player stood — Fly resumes from there.
}

// --- B3/B4: the level editor -------------------------------------------------------

Vec3 LandscapeScene::mouseRayDirection(const Vec2& mousePx) const {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const f32 aspect = display.y > 0.0f ? display.x / display.y : 1.0f;
    const Vec2 ndc { 2.0f * mousePx.x / display.x - 1.0f,
                     1.0f - 2.0f * mousePx.y / display.y };
    const Mat4 inv =
        glm::inverse(flyCamera.camera.viewProj(aspect));
    Vec4 nearPoint = inv * Vec4 { ndc.x, ndc.y, -1.0f, 1.0f };
    Vec4 farPoint = inv * Vec4 { ndc.x, ndc.y, 1.0f, 1.0f };
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    return glm::normalize(Vec3 { farPoint } - Vec3 { nearPoint });
}

bool LandscapeScene::pickEntity(const Vec2& mousePx, ecs::Entity& out) {
    const Vec3 origin = flyCamera.camera.position;
    const Vec3 dir = mouseRayDirection(mousePx);
    f32 bestT = 1e9f;
    ecs::Entity best {};
    world.handle()
        .query<const world::Transform, const world::MeshRender,
               const world::RefId>()
        .each([&](flecs::entity e, const world::Transform& transform,
                  const world::MeshRender& mesh, const world::RefId&) {
            Vec3 lo { -0.5f }, hi { 0.5f };
            if (const MeshCache::CpuMesh* cpu =
                    meshCache->cpuMesh(mesh.model)) {
                lo = cpu->boundsMin;
                hi = cpu->boundsMax;
            }
            // World AABB of the transformed local box (8 corners).
            const Mat4 model =
                glm::translate(Mat4 { 1.0f }, transform.position) *
                glm::mat4_cast(transform.rotation) *
                glm::scale(Mat4 { 1.0f }, transform.scale);
            Vec3 wlo { 1e9f }, whi { -1e9f };
            for (u32 i = 0; i < 8; ++i) {
                const Vec3 corner { (i & 1) ? hi.x : lo.x,
                                    (i & 2) ? hi.y : lo.y,
                                    (i & 4) ? hi.z : lo.z };
                const Vec3 world3 = Vec3 { model * Vec4 { corner, 1.0f } };
                wlo = glm::min(wlo, world3);
                whi = glm::max(whi, world3);
            }
            // Slab test.
            f32 t0 = 0.0f, t1 = 1e9f;
            for (u32 axis = 0; axis < 3; ++axis) {
                const f32 d = dir[static_cast<i32>(axis)];
                const f32 o = origin[static_cast<i32>(axis)];
                if (std::abs(d) < 1e-6f) {
                    if (o < wlo[static_cast<i32>(axis)] ||
                        o > whi[static_cast<i32>(axis)]) {
                        return;
                    }
                    continue;
                }
                f32 near = (wlo[static_cast<i32>(axis)] - o) / d;
                f32 far = (whi[static_cast<i32>(axis)] - o) / d;
                if (near > far) {
                    std::swap(near, far);
                }
                t0 = glm::max(t0, near);
                t1 = glm::min(t1, far);
                if (t0 > t1) {
                    return;
                }
            }
            if (t0 < bestT) {
                bestT = t0;
                best = ecs::Entity { e };
            }
        });
    out = best;
    return best.is_alive();
}

bool LandscapeScene::groundUnderMouse(const Vec2& mousePx, Vec3& out) {
    const Vec3 origin = flyCamera.camera.position;
    const Vec3 dir = mouseRayDirection(mousePx);
    if (interiorMode) { // interiors: intersect the y = 0 floor plane
        if (std::abs(dir.y) < 1e-4f) {
            return false;
        }
        const f32 t = -origin.y / dir.y;
        if (t <= 0.0f || t > 200.0f) {
            return false;
        }
        out = origin + dir * t;
        return true;
    }
    // Raymarch the height function: coarse steps, then a refinement.
    f32 t = 0.0f;
    f32 previous = t;
    for (u32 i = 0; i < 400; ++i) {
        t += 1.5f;
        const Vec3 p = origin + dir * t;
        if (p.y <= render::terrain::height(terrain.params, p.x, p.z)) {
            for (u32 r = 0; r < 12; ++r) { // bisect
                const f32 mid = (previous + t) * 0.5f;
                const Vec3 m = origin + dir * mid;
                if (m.y <=
                    render::terrain::height(terrain.params, m.x, m.z)) {
                    t = mid;
                } else {
                    previous = mid;
                }
            }
            const Vec3 hit = origin + dir * t;
            out = { hit.x,
                    render::terrain::height(terrain.params, hit.x, hit.z),
                    hit.z };
            return true;
        }
        previous = t;
    }
    return false;
}

// --- B9: terrain sculpt ------------------------------------------------------------

render::HeightPatch& LandscapeScene::sculptGridFor(i32 cx, i32 cz) {
    const u64 key = render::HeightPatches::keyOf(cx, cz);
    const auto it = sculptGrids.find(key);
    if (it != sculptGrids.end()) {
        return it->second;
    }
    // Seed from the published overlay when the chunk is already authored.
    if (heightPatches) {
        if (const auto existing = heightPatches->chunks.find(key);
            existing != heightPatches->chunks.end()) {
            return sculptGrids.emplace(key, existing->second).first->second;
        }
    }
    render::HeightPatch fresh;
    fresh.samples = 65;
    fresh.deltas.assign(65 * 65, 0.0f);
    return sculptGrids.emplace(key, std::move(fresh)).first->second;
}

void LandscapeScene::applyBrush(const Vec3& center, f32 dt) {
    constexpr f32 kChunk = 64.0f;
    const i32 minCx = static_cast<i32>(
        std::floor((center.x - brushRadius) / kChunk));
    const i32 maxCx = static_cast<i32>(
        std::floor((center.x + brushRadius) / kChunk));
    const i32 minCz = static_cast<i32>(
        std::floor((center.z - brushRadius) / kChunk));
    const i32 maxCz = static_cast<i32>(
        std::floor((center.z + brushRadius) / kChunk));
    for (i32 cz = minCz; cz <= maxCz; ++cz) {
        for (i32 cx = minCx; cx <= maxCx; ++cx) {
            render::HeightPatch& grid = sculptGridFor(cx, cz);
            for (u32 row = 0; row < grid.samples; ++row) {
                for (u32 col = 0; col < grid.samples; ++col) {
                    const f32 x =
                        static_cast<f32>(cx) * kChunk + static_cast<f32>(col);
                    const f32 z =
                        static_cast<f32>(cz) * kChunk + static_cast<f32>(row);
                    const f32 dx = x - center.x;
                    const f32 dz = z - center.z;
                    const f32 dist = std::sqrt(dx * dx + dz * dz);
                    if (dist >= brushRadius) {
                        continue;
                    }
                    const f32 t = 1.0f - dist / brushRadius;
                    const f32 falloff = t * t * (3.0f - 2.0f * t);
                    f32& delta = grid.deltas[row * grid.samples + col];
                    switch (brushKind) {
                    case 0: // raise
                        delta += brushStrength * falloff * dt;
                        break;
                    case 1: // lower
                        delta -= brushStrength * falloff * dt;
                        break;
                    case 2: { // flatten toward the stroke-start height:
                        // work against the LIVE height (base + published
                        // patch); the working delta absorbs the gap.
                        const f32 current =
                            render::terrain::height(terrain.params, x, z);
                        const f32 gap = flattenTarget - current;
                        delta += gap * glm::min(2.5f * falloff * dt, 1.0f);
                        break;
                    }
                    case 3: { // smooth: relax toward the neighbour average
                        const u32 c0 = col > 0 ? col - 1 : col;
                        const u32 c1 = glm::min(col + 1, grid.samples - 1);
                        const u32 r0 = row > 0 ? row - 1 : row;
                        const u32 r1 = glm::min(row + 1, grid.samples - 1);
                        const f32 average =
                            (grid.deltas[row * grid.samples + c0] +
                             grid.deltas[row * grid.samples + c1] +
                             grid.deltas[r0 * grid.samples + col] +
                             grid.deltas[r1 * grid.samples + col]) *
                            0.25f;
                        delta += (average - delta) *
                                 glm::min(4.0f * falloff * dt, 1.0f);
                        break;
                    }
                    default:
                        break;
                    }
                }
            }
        }
    }
}

void LandscapeScene::publishSculpt() {
    if (sculptGrids.empty()) {
        return;
    }
    // New immutable overlay = published chunks overridden by the working
    // grids; in-flight workers keep the old instance alive through their
    // copied TerrainParams (shared_ptr).
    auto next = std::make_shared<render::HeightPatches>();
    next->chunkSize = 64.0f;
    if (heightPatches) {
        next->chunks = heightPatches->chunks;
    }
    for (const auto& [key, grid] : sculptGrids) {
        next->chunks[key] = grid;
    }
    heightPatches = std::move(next);
    terrain.params.patches = heightPatches;
    regenerateRequested = true; // terrain + scatter rebuild next frame
    occlusion.invalidate();
    terrainCollision = std::make_unique<TerrainCollision>(
        *physics, terrain.params, &engine->getJobSystem());
    vegCollision =
        std::make_unique<VegetationCollision>(*physics, terrain.params);
    snapCellEntities();
}

void LandscapeScene::saveSculptToMod() {
    const auto dir = platform::executableDir() / "data" / "mods" / "terrain";
    std::error_code errc;
    std::filesystem::create_directories(dir, errc);
    const reflect::TypeInfo& type = world::TerrainPatchForm::staticTypeInfo();
    for (const auto& [key, grid] : sculptGrids) {
        const i32 cx = static_cast<i32>(key >> 32);
        const i32 cz = static_cast<i32>(key & 0xffffffffu);
        char name[64];
        std::snprintf(name, sizeof(name), "patch_%d_%d.ter", cx, cz);
        if (!world::writeTerFile(dir / name, grid)) {
            continue;
        }
        // Deterministic asset guid per chunk (stable across saves).
        char guidText[40];
        std::snprintf(guidText, sizeof(guidText),
                      "7e88a110-0000-4000-8000-%012llx",
                      static_cast<unsigned long long>(key & 0xFFFFFFFFFFFFull));
        const core::Guid assetGuid = *core::Guid::fromString(guidText);
        levelEditor->addExportAsset(assetGuid, str { "terrain/" } + name);
        // One TerrainPatchForm per chunk — reuse the existing record if
        // this chunk was already authored (patch it), else create.
        core::Guid recordGuid {};
        data::forEach<world::TerrainPatchForm>(
            forms, [&](const world::TerrainPatchForm& form) {
                if (form.chunkX == cx && form.chunkZ == cz) {
                    recordGuid = form.id;
                }
            });
        auto& session = levelEditor->editSession();
        if (!recordGuid.isValid()) {
            char editorId[64];
            std::snprintf(editorId, sizeof(editorId), "SculptPatch_%d_%d",
                          cx, cz);
            recordGuid = session.createForm(type.id, editorId);
            session.setField(recordGuid, type.findField("chunkX")->id,
                             reflect::Value { cx });
            session.setField(recordGuid, type.findField("chunkZ")->id,
                             reflect::Value { cz });
        }
        session.setField(recordGuid, type.findField("asset")->id,
                         reflect::Value { assetGuid });
    }
    LOG_INFO("B9: {} sculpted chunk(s) staged — Export writes the mod",
             sculptGrids.size());
}

// The editor frame: gizmo on the selection, click-to-pick / click-to-place,
// and the editor window (palette, selection ops, session, export).
void LandscapeScene::drawEditorUi() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    const f32 aspect = display.y > 0.0f ? display.x / display.y : 1.0f;
    ImGuizmo::BeginFrame();
    ImGuizmo::SetRect(0.0f, 0.0f, display.x, display.y);

    // Gizmo op hotkeys (1/2/3), like every DCC.
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
        gizmoOperation = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) {
        gizmoOperation = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
        gizmoOperation = 2;
    }

    // Gizmo on the live selection; commit to the RECORD on release.
    if (editSelection.is_alive()) {
        auto& transform = editSelection.get_mut<world::Transform>();
        Mat4 model = glm::translate(Mat4 { 1.0f }, transform.position) *
                     glm::mat4_cast(transform.rotation) *
                     glm::scale(Mat4 { 1.0f }, transform.scale);
        const Mat4 view = flyCamera.camera.view();
        const Mat4 proj = flyCamera.camera.proj(aspect);
        const ImGuizmo::OPERATION op =
            gizmoOperation == 0   ? ImGuizmo::TRANSLATE
            : gizmoOperation == 1 ? ImGuizmo::ROTATE
                                  : ImGuizmo::SCALE;
        if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], op,
                                 ImGuizmo::WORLD, &model[0][0])) {
            // Manual decompose (translation / per-column scale / rotation).
            transform.position = Vec3 { model[3] };
            Vec3 scale { glm::length(Vec3 { model[0] }),
                         glm::length(Vec3 { model[1] }),
                         glm::length(Vec3 { model[2] }) };
            scale = glm::max(scale, Vec3 { 1e-4f });
            transform.scale = scale;
            transform.rotation = glm::normalize(glm::quat_cast(
                Mat3 { Vec3 { model[0] } / scale.x,
                       Vec3 { model[1] } / scale.y,
                       Vec3 { model[2] } / scale.z }));
        }
        const bool usingNow = ImGuizmo::IsUsing();
        if (gizmoWasUsing && !usingNow) {
            levelEditor->commitTransform(
                editSelection.get<world::RefId>().referenceId,
                transform.position, transform.rotation, transform.scale);
        }
        gizmoWasUsing = usingNow;
    } else {
        gizmoWasUsing = false;
    }

    // Sculpt strokes take priority over pick/place while armed.
    if (sculptMode && !io.WantCaptureMouse && !flyCamera.capturing()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            Vec3 ground;
            if (groundUnderMouse({ io.MousePos.x, io.MousePos.y }, ground)) {
                if (!strokeActive) {
                    strokeActive = true;
                    flattenTarget = ground.y;
                }
                applyBrush(ground, io.DeltaTime);
            }
        } else if (strokeActive) {
            strokeActive = false;
            publishSculpt(); // rebuild once per stroke, not per frame
        }
    }

    // Click: place (armed palette entry) or pick. Never while the mouse is
    // over a window/gizmo, while mouselooking, or while sculpting.
    if (!sculptMode && !io.WantCaptureMouse && !ImGuizmo::IsOver() &&
        !flyCamera.capturing() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const Vec2 mouse { io.MousePos.x, io.MousePos.y };
        if (placementBase.isValid()) {
            Vec3 ground;
            if (groundUnderMouse(mouse, ground)) {
                // The authored cell under the hit (placement needs one).
                core::Guid cellGuid {};
                if (const auto* space =
                        static_cast<const world::WorldspaceForm*>(
                            forms.get(activeWorldspace))) {
                    const i32 gx = static_cast<i32>(
                        std::floor(ground.x / space->cellSize));
                    const i32 gy = static_cast<i32>(
                        std::floor(ground.z / space->cellSize));
                    const data::FormHandle cell =
                        worldModel.cellAt(activeWorldspace, gx, gy);
                    if (const data::Form* form = forms.get(cell)) {
                        cellGuid = form->id;
                    }
                }
                if (!cellGuid.isValid()) {
                    LOG_WARN("Editor: no authored cell here — placement "
                             "aborted");
                } else {
                    // Authored y follows the base's convention: snapping
                    // bases store an offset (0 = on the ground), pad-based
                    // ones (snapToGround = false) store the ABSOLUTE hit.
                    f32 storedY = 0.0f;
                    if (const data::Form* baseF = forms.find(placementBase)) {
                        const reflect::TypeInfo* baseT =
                            forms.typeOf(forms.handleOf(placementBase));
                        if (const reflect::FieldInfo* field =
                                baseT ? baseT->findField("snapToGround")
                                      : nullptr;
                            field &&
                            field->kind == reflect::FieldKind::Bool &&
                            !std::get<bool>(field->get(baseF))) {
                            storedY = ground.y;
                        }
                    }
                    const core::Guid placed = levelEditor->placeReference(
                        placementBase, cellGuid,
                        { ground.x, storedY, ground.z });
                    // Live spawn from the draft, into the loaded cell.
                    if (const auto* draft =
                            static_cast<const world::ReferenceForm*>(
                                levelEditor->editSession().view(placed))) {
                        world::SpawnContext ctx { world, forms, categories };
                        world::ReferenceForm live = *draft;
                        live.position = ground; // grounded live position
                        const ecs::Entity cellEntity = cellLoader->cellEntity(
                            forms.handleOf(cellGuid));
                        const ecs::Entity entity =
                            spawner.spawn(ctx, live, cellEntity);
                        if (entity.is_alive()) {
                            editSelection = entity;
                            levelEditor->select(placed);
                        }
                    }
                }
            }
        } else {
            ecs::Entity picked {};
            if (pickEntity(mouse, picked)) {
                editSelection = picked;
                levelEditor->select(
                    picked.get<world::RefId>().referenceId);
                if (io.KeyCtrl) { // Ctrl+click: grow the prefab group
                    levelEditor->groupSelection().push_back(
                        picked.get<world::RefId>().referenceId);
                }
            } else {
                editSelection = ecs::Entity {};
                levelEditor->select(core::Guid {});
            }
        }
    }

    // The editor window.
    ImGui::Begin("Level editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted(
        "LMB: pick / place | Ctrl+LMB: add to group | 1/2/3: gizmo op\n"
        "Hold LMB on sky + WASD: fly | F6: leave editor");
    ImGui::Text("Session: %u dirty record(s)",
                levelEditor->editSession().dirtyCount());
    if (ImGui::Button("Undo") && levelEditor->editSession().canUndo()) {
        levelEditor->editSession().undo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Redo") && levelEditor->editSession().canRedo()) {
        levelEditor->editSession().redo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Export mod (data/mods/level-edits.toml)")) {
        levelEditor->exportTo(platform::executableDir() / "data" / "mods" /
                                  "level-edits.toml",
                              *core::Guid::fromString(
                                  "aaaaaaaa-0000-4000-8000-0000000000ed"),
                              "level-edits");
    }
    if (editSelection.is_alive()) {
        const auto& ref = editSelection.get<world::RefId>();
        ImGui::Separator();
        ImGui::Text("Selected: %s", ref.referenceId.toString().c_str());
        if (ImGui::Button("Disable (delete)")) {
            levelEditor->disableReference(ref.referenceId);
            editSelection.destruct();
            editSelection = ecs::Entity {};
        }
    }
    ImGui::Separator();
    ImGui::Text("Prefab group: %u",
                static_cast<u32>(levelEditor->groupSelection().size()));
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        levelEditor->groupSelection().clear();
    }
    static char prefabName[64] = "MyPrefab";
    ImGui::InputText("##prefabname", prefabName, sizeof(prefabName));
    ImGui::SameLine();
    if (ImGui::Button("Create prefab from group")) {
        const core::Guid instance = levelEditor->createPrefabFromSelection(
            levelEditor->groupSelection(), prefabName);
        if (instance.isValid()) {
            // Remove the originals' live entities; spawn the instance.
            for (const core::Guid& id : levelEditor->groupSelection()) {
                world.handle()
                    .query<const world::RefId>()
                    .each([&](flecs::entity e, const world::RefId& rid) {
                        if (rid.referenceId == id) {
                            ecs::Entity { e }.destruct();
                        }
                    });
            }
            levelEditor->groupSelection().clear();
            if (const auto* draft =
                    static_cast<const world::ReferenceForm*>(
                        levelEditor->editSession().view(instance))) {
                world::SpawnContext ctx { world, forms, categories };
                spawner.spawn(ctx, *draft, ecs::Entity {});
            }
        }
    }
    ImGui::Separator();
    // B9: terrain sculpt.
    ImGui::Checkbox("Sculpt terrain", &sculptMode);
    if (sculptMode) {
        ImGui::Combo("Brush", &brushKind, "Raise\0Lower\0Flatten\0Smooth\0");
        ImGui::SliderFloat("Radius (m)", &brushRadius, 1.0f, 24.0f, "%.0f");
        ImGui::SliderFloat("Strength", &brushStrength, 0.2f, 10.0f, "%.1f");
        ImGui::Text("Sculpted chunks: %u",
                    static_cast<u32>(sculptGrids.size()));
        if (ImGui::Button("Save terrain to mod")) {
            saveSculptToMod();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(then Export)");
    }
    ImGui::Separator();
    ImGui::TextUnformatted(placementBase.isValid()
                               ? "Placing: click the ground (Esc: cancel)"
                               : "Palette — click to arm:");
    if (placementBase.isValid() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        placementBase = core::Guid {};
    }
    // Palette: every placeable base form, by category.
    const auto paletteEntry = [&](const data::Form& form) {
        const bool armed = placementBase == form.id;
        if (ImGui::Selectable(
                (form.editorId + (armed ? "  [armed]" : "")).c_str(),
                armed)) {
            placementBase = armed ? core::Guid {} : form.id;
        }
    };
    if (ImGui::CollapsingHeader("Statics",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        data::forEach<data::StaticForm>(
            forms, [&](const data::StaticForm& form) {
                paletteEntry(form);
            });
    }
    if (ImGui::CollapsingHeader("Lights")) {
        data::forEach<data::LightForm>(
            forms,
            [&](const data::LightForm& form) { paletteEntry(form); });
    }
    if (ImGui::CollapsingHeader("Prefabs")) {
        data::forEach<world::PrefabForm>(
            forms,
            [&](const world::PrefabForm& form) { paletteEntry(form); });
    }
    ImGui::End();
}

// --- B7: doors & worldspace travel ------------------------------------------------

void LandscapeScene::updateInteraction(f32 dt) {
    promptEntity = ecs::Entity {};
    promptKind = PromptKind::None;
    if (talkTimer > 0.0f) {
        talkTimer -= dt;
    }
    if (fadeDirection == 0 && playMode && player) {
        // Aim test: nearest interactable within reach, roughly in front
        // of the eye. One scorer for every kind.
        const Vec3 eye = player->position() + Vec3 { 0.0f, 1.7f, 0.0f };
        const Vec3 forward = flyCamera.camera.forward();
        f32 bestScore = 0.55f; // minimum facing alignment
        const auto consider = [&](flecs::entity e, const Vec3& position,
                                  PromptKind kind, f32 reach) {
            const Vec3 to = position + Vec3 { 0.0f, 1.1f, 0.0f } - eye;
            const f32 distance = glm::length(to);
            if (distance > reach || distance < 1e-3f) {
                return;
            }
            const f32 facing = glm::dot(to / distance, forward);
            if (facing > bestScore) {
                bestScore = facing;
                promptEntity = ecs::Entity { e };
                promptKind = kind;
            }
        };
        doorQuery.each([&](flecs::entity e,
                           const world::Transform& transform,
                           const world::DoorTarget&) {
            consider(e, transform.position, PromptKind::Door, 3.0f);
        });
        interactQuery.each([&](flecs::entity e,
                               const world::Transform& transform,
                               const world::RefId&) {
                const ecs::Entity entity { e };
                if (entity.has<world::ItemMarker>()) {
                    consider(e, transform.position, PromptKind::Item, 2.4f);
                } else if (entity.has<world::ActorMarker>() &&
                           entity != playerEntity) {
                    // Chantier 4 B3: a dead actor is searched, not talked to.
                    bool isDead = false;
                    for (const auto& npc : npcs) {
                        if (npc->entity == entity) {
                            isDead = npc->dead;
                            break;
                        }
                    }
                    consider(e, transform.position,
                             isDead ? PromptKind::Corpse : PromptKind::Actor,
                             2.8f);
                } else if (entity.has<world::FurnitureMarker>()) {
                    consider(e, transform.position, PromptKind::Furniture,
                             2.4f);
                }
            });

        // The prompt label from the base form's displayName (reflection).
        promptLabel.clear();
        if (promptEntity.is_alive()) {
            str name;
            const auto& ref = promptEntity.get<world::RefId>();
            if (const data::Form* base = forms.get(ref.base)) {
                if (const reflect::TypeInfo* type = forms.typeOf(ref.base)) {
                    if (const reflect::FieldInfo* field =
                            type->findField("displayName");
                        field && field->kind == reflect::FieldKind::Str) {
                        name = std::get<str>(field->get(base));
                    }
                }
            }
            switch (promptKind) {
            case PromptKind::Door:
                promptLabel = "[E] " + (name.empty() ? "Use door" : name);
                break;
            case PromptKind::Item:
                promptLabel =
                    "[E] Take " + (name.empty() ? "item" : name);
                break;
            case PromptKind::Actor:
                promptLabel =
                    "[E] Talk to " + (name.empty() ? "them" : name);
                break;
            case PromptKind::Corpse:
                promptLabel =
                    "[E] Search " + (name.empty() ? "the body" : name);
                break;
            case PromptKind::Furniture:
                promptLabel = "[E] Use " + (name.empty() ? "this" : name);
                break;
            default:
                break;
            }
        }

        if (promptEntity.is_alive() &&
            engine->getInput().wasPressed(platform::Key::E)) {
            switch (promptKind) {
            case PromptKind::Door:
                pendingTravel =
                    promptEntity.get<world::DoorTarget>().targetReference;
                fadeDirection = 1;
                break;
            case PromptKind::Item: {
                // Into the inventory; the entity leaves the world and the
                // PENDING layer remembers (chantier 5 B4): the reference
                // stays disabled when its cell reloads, and the disk save
                // flushes enabled = false.
                const auto& ref = promptEntity.get<world::RefId>();
                if (const data::Form* base = forms.get(ref.base)) {
                    if (!playerEntity.has<gameplay::Inventory>()) {
                        playerEntity.set<gameplay::Inventory>({});
                    }
                    gameplay::addItem(
                        playerEntity.get_mut<gameplay::Inventory>(),
                        base->id, 1);
                    LOG_INFO("Taken: {}", base->editorId);
                }
                if (ref.referenceId.isValid()) {
                    pendingSave.disableReference(ref.referenceId, forms,
                                                 promptEntity);
                }
                promptEntity.destruct();
                promptEntity = ecs::Entity {};
                break;
            }
            case PromptKind::Actor: {
                // B4: actors with a DialogueForm talk for real; the rest
                // keep the placeholder line.
                const auto& ref = promptEntity.get<world::RefId>();
                const data::Form* base = forms.get(ref.base);
                const reflect::TypeInfo* type = forms.typeOf(ref.base);
                const auto* actor =
                    base && type &&
                            type->isA(data::ActorForm::staticTypeInfo().id)
                        ? static_cast<const data::ActorForm*>(base)
                        : nullptr;
                if (actor && actor->dialogue.isValid()) {
                    dialoguePartner = promptEntity; // the vendor for B5
                    openDialogue(actor->dialogue);
                } else {
                    talkLine = "Belle journee, voyageur.";
                    talkTimer = 4.0f;
                }
                break;
            }
            case PromptKind::Corpse:
                openContainerScreen(promptEntity);
                break;
            case PromptKind::Furniture: {
                // B7-lite: beds sleep 8h, seats rest 1h — both through the
                // Phase-7 gameplay::sleep() at the black of the fade.
                // Chantier 4 B6: a WORKSTATION opens its UI screen instead
                // (FurnitureForm.screen — crafting tables are furniture +
                // a screen).
                const auto& ref = promptEntity.get<world::RefId>();
                f32 hours = 1.0f;
                str screen;
                if (const reflect::TypeInfo* type = forms.typeOf(ref.base);
                    type &&
                    type->isA(gameplay::FurnitureForm::staticTypeInfo().id)) {
                    const auto* furniture = static_cast<
                        const gameplay::FurnitureForm*>(forms.get(ref.base));
                    if (furniture->category == "bed") { hours = 8.0f; }
                    screen = furniture->screen;
                }
                if (!screen.empty() && screenStack.find(screen)) {
                    screenStack.show(screen);
                } else {
                    pendingSleepHours = hours;
                    fadeDirection = 1;
                }
                break;
            }
            default:
                break;
            }
        }
    }
    // Fade state machine: out (0.3 s) -> travel at black -> in.
    constexpr f32 kFadeSpeed = 1.0f / 0.3f;
    if (fadeDirection > 0) {
        fadeAlpha += dt * kFadeSpeed;
        if (fadeAlpha >= 1.0f) {
            fadeAlpha = 1.0f;
            if (pendingSleepHours > 0.0f) {
                performRest(pendingSleepHours);
                pendingSleepHours = 0.0f;
            } else {
                performTravel(pendingTravel);
                pendingTravel = core::Guid {};
            }
            fadeDirection = -1;
        }
    } else if (fadeDirection < 0) {
        // Hold at black until the world is SOLID under the player: after
        // a travel the arrival cell's meshes may still be decoding and
        // the collider cook is budgeted — fading in before the floor
        // exists dropped the player through it (dev report 2026-07-07).
        // Timeout keeps an authoring hole from freezing the game black.
        if (fadeAlpha >= 1.0f && playMode && player && physics) {
            fadeHoldSeconds += dt;
            const phys::RayHit floor = physics->rayCast(
                player->position() + Vec3 { 0.0f, 0.5f, 0.0f },
                { 0.0f, -1.0f, 0.0f }, 6.0f);
            if (!floor.hit && fadeHoldSeconds < 5.0f) {
                return; // still cooking — stay black, player stays frozen
            }
        }
        fadeHoldSeconds = 0.0f;
        fadeAlpha -= dt * kFadeSpeed;
        if (fadeAlpha <= 0.0f) {
            fadeAlpha = 0.0f;
            fadeDirection = 0;
        }
    }
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
    snapCellEntities();
    refreshNpcs(engine->getDevice());
    updateStaticColliders();
    refreshNavObstacles();

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
    if (player) {
        player = std::make_unique<phys::CharacterBody>(
            *physics, 0.3f, 1.8f,
            marker->position + Vec3 { 0.0f, 0.25f, 0.0f });
        playerVelocity = Vec3 { 0.0f };
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

// Chantier 3 B7-lite: rest/sleep on furniture — the Phase-7 sleep()
// advances the game clock (the sky follows on the next frame), decays
// hunger/thirst over the skipped time, restores the sleep need, and
// accrues Rest (the injury/resonance recovery precondition). NPC
// schedules re-evaluate on their next slot check and warp forward.
void LandscapeScene::performRest(f32 hours) {
    if (!playerEntity.is_alive()) {
        return;
    }
    if (!playerEntity.has<gameplay::Survival>() ||
        !playerEntity.has<gameplay::CombatState>()) {
        LOG_WARN("B7: player has no survival stats; rest skipped");
        return;
    }
    gameplay::sleep(gameClock,
                    playerEntity.get_mut<gameplay::Survival>(),
                    playerEntity.get_mut<gameplay::CombatState>(),
                    hours, statsTuning);
    talkLine = hours >= 8.0f
        ? "Vous dormez profondement (8 h)."
        : "Vous vous reposez un moment (1 h).";
    talkTimer = 3.0f;
    LOG_INFO("B7-lite: rested {} h -> game time {:.2f} h", hours,
             std::fmod(gameClock.gameHours(), 24.0));
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
            handleUiEvent(model, event, args);
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
        updateMenuClockLine();
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
        } else if (!screenStack.modalOpen() && playMode &&
                   screenStack.find("pause")) {
            updateMenuClockLine();
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
            openInventoryScreen();
        }
    }
    // T: the wait menu (B6) — Play only, nothing else open.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() && playMode &&
        input.wasPressed(platform::Key::T) && !screenStack.modalOpen()) {
        updateMenuClockLine();
        screenStack.show("wait");
    }
    // J: the quest journal (chantier 6 A3) — the I-key idiom.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() &&
        input.wasPressed(platform::Key::J)) {
        const ScreenStack::Screen* top = screenStack.topModal();
        if (top && top->name == "journal") {
            screenStack.closeTop();
        } else if (!screenStack.modalOpen()) {
            pushJournalModel();
            screenStack.show("journal");
        }
    }

    const bool modal = screenStack.modalOpen();
    if (modal != uiModalWasOpen) {
        // A modal frees the mouse (and pauses the sim, handled in
        // update()); closing it restores the Play capture.
        if (playMode) {
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
            if (search != invView.search()) {
                invView.setSearch(search);
                pushItemModels();
            }
        }
    }

    updateHudModel();
    syncScreens();
    uiSystem.update(dt);
}

void LandscapeScene::updateHudModel() {
    if (!uiCreated) {
        return;
    }
    if (playerEntity.is_alive() &&
        playerEntity.has<gameplay::AbilitySystem>() &&
        playerEntity.has<gameplay::AttributeSet>()) {
        const auto& sys = playerEntity.get<gameplay::AbilitySystem>();
        const auto& vitals = playerEntity.get<gameplay::AttributeSet>();
        const auto pct = [](f32 value, f32 max) {
            return max > 0.0f ? glm::clamp(100.0f * value / max, 0.0f,
                                           100.0f)
                              : 0.0f;
        };
        const f32 maxHealth =
            gameplay::currentValueOf(sys, gameplay::attr("maxHealth"));
        const f32 maxEnergy =
            gameplay::currentValueOf(sys, gameplay::attr("maxEnergy"));
        const f32 maxEssence =
            gameplay::currentValueOf(sys, gameplay::attr("maxEssence"));
        const f32 maxPosture =
            gameplay::currentValueOf(sys, gameplay::attr("maxPosture"));
        uiSystem.setNumber("hud", "healthPct", pct(vitals.health, maxHealth));
        uiSystem.setNumber("hud", "energyPct", pct(vitals.energy, maxEnergy));
        uiSystem.setNumber("hud", "essencePct",
                           pct(vitals.essence, maxEssence));
        f32 posture = maxPosture;
        if (playerEntity.has<gameplay::CombatState>()) {
            posture = playerEntity.get<gameplay::CombatState>().posture;
        }
        uiSystem.setNumber("hud", "posturePct", pct(posture, maxPosture));
        const auto text = [](f32 value, f32 max) {
            return std::to_string(static_cast<i32>(value + 0.5f)) + " / " +
                   std::to_string(static_cast<i32>(max + 0.5f));
        };
        uiSystem.setString("hud", "healthText", text(vitals.health, maxHealth));
        uiSystem.setString("hud", "energyText", text(vitals.energy, maxEnergy));
        uiSystem.setString("hud", "essenceText",
                           text(vitals.essence, maxEssence));
    }
    const f64 hours = std::fmod(gameClock.gameHours(), 24.0);
    const i32 hh = static_cast<i32>(hours);
    const i32 mm = static_cast<i32>((hours - hh) * 60.0);
    char clock[8];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", hh, mm);
    uiSystem.setString("hud", "clock", clock);
    // The interaction prompt + talk line (migrated from the ImGui overlay).
    const bool promptOn = playMode && promptEntity.is_alive() &&
                          fadeDirection == 0 && !promptLabel.empty();
    uiSystem.setBool("hud", "promptVisible", promptOn);
    uiSystem.setString("hud", "prompt", promptOn ? promptLabel : str {});
    const bool talkOn = talkTimer > 0.0f && !talkLine.empty();
    uiSystem.setBool("hud", "talkVisible", talkOn);
    uiSystem.setString("hud", "talk", talkOn ? talkLine : str {});
    updateNameplates(); // B7
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

void LandscapeScene::performSave(const str& slot) {
    // Capture EVERYTHING live (loaded cells' entities + the persistent
    // player) into the pending layer, then flush it plus the world state
    // into one ordinary plugin (§5). Sweep order is the flush's sorted
    // order — deterministic (§8).
    vector<ecs::Entity> live;
    interactQuery.each([&](flecs::entity e, const world::Transform&,
                           const world::RefId&) {
        live.push_back(ecs::Entity { e });
    });
    for (ecs::Entity entity : live) {
        pendingSave.captureEntity(entity, forms, gameTags);
    }

    data::Plugin plugin;
    plugin.id = *core::Guid::fromString(
        "5a5e0000-0000-4000-8000-000000000001"); // the one save layer
    plugin.name = "save-" + slot;
    plugin.records = pendingSave.flush();
    // Chantier 6 A4: the quest log (scene-level, rebuilt fresh each save
    // like the WorldStateForm — never in the pending layer).
    const auto questRecords = quest::captureQuestLog(questLog);
    plugin.records.insert(plugin.records.end(), questRecords.begin(),
                          questRecords.end());

    gameplay::WorldStateForm state;
    state.gameSeconds = gameClock.gameSeconds;
    state.timescale = gameClock.timescale;
    if (activeWorldspace.isValid()) {
        if (const data::Form* space = forms.get(activeWorldspace)) {
            state.activeWorldspace = space->id;
        }
    }
    state.playerYaw = flyCamera.camera.yaw;
    state.playerPitch = flyCamera.camera.pitch;
    state.playMode = playMode;
    state.weatherSelected = weatherSelected;
    plugin.records.push_back(gameplay::createRecord(
        state, *core::Guid::fromString(
                   "5a5e0000-0000-4000-8000-0000000000ff")));

    if (writeSave(slot, plugin, formTypes)) {
        talkLine = "Partie sauvegardee (" + slot + ").";
        talkTimer = 3.0f;
    }
}

void LandscapeScene::requestLoad(const str& slot) {
    if (!std::filesystem::exists(savePath(slot))) {
        talkLine = "Aucune sauvegarde '" + slot + "'.";
        talkTimer = 3.0f;
        return;
    }
    pendingLoadSlot = slot;
    reloadRequested = true; // consumed at the end of update()
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
    // Pending layer first (a cell reloading in THIS session), then the
    // resolved database (a loaded save). The SavedStatsForm existence is
    // the sentinel — a captured actor never re-rolls its loadout (§8).
    if (pendingSave.hasActorState(refGuid)) {
        gameplay::applySavedState(entity, pendingSave.actorState(refGuid),
                                  gameTags);
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

// --- Chantier 4 B3: inventory / container ------------------------------------------

void LandscapeScene::openInventoryScreen() {
    containerEntity = ecs::Entity {};
    barterMode = false;
    uiSystem.setBool("inventory", "transferMode", false);
    pushItemModels();
    screenStack.show("inventory");
}

void LandscapeScene::openContainerScreen(ecs::Entity container) {
    containerEntity = container;
    barterMode = false;
    if (containerEntity.is_alive() &&
        !containerEntity.has<gameplay::Inventory>()) {
        containerEntity.set<gameplay::Inventory>({});
    }
    uiSystem.setBool("inventory", "transferMode", true);
    pushItemModels();
    screenStack.show("container");
}

void LandscapeScene::openBarterScreen(ecs::Entity vendor) {
    if (!vendor.is_alive() || !goldForm || !playerEntity.is_alive()) {
        return;
    }
    containerEntity = vendor;
    barterMode = true;
    if (!containerEntity.has<gameplay::Inventory>()) {
        containerEntity.set<gameplay::Inventory>({});
    }
    uiSystem.setBool("inventory", "transferMode", true);
    // The vendor's name for the title + its barter profile (D1).
    str title = "Merchant";
    vendorBuyMult = statsTuning.barterBuyMult;
    vendorSellMult = statsTuning.barterSellMult;
    core::Guid vendorFormId;
    if (containerEntity.has<world::RefId>()) {
        const auto& ref = containerEntity.get<world::RefId>();
        if (const reflect::TypeInfo* type = forms.typeOf(ref.base);
            type && type->isA(data::ActorForm::staticTypeInfo().id)) {
            const auto* actor =
                static_cast<const data::ActorForm*>(forms.get(ref.base));
            if (!actor->displayName.empty()) {
                title = actor->displayName;
            }
            if (actor->buyMult > 0.0f) {
                vendorBuyMult = actor->buyMult;
            }
            if (actor->sellMult > 0.0f) {
                vendorSellMult = actor->sellMult;
            }
            vendorFormId = actor->id;
        }
    }
    uiSystem.setString("barter", "title", title);

    // D1: restock — more than kRestockHours of game time since the last
    // re-roll = clear + a fresh loadout roll (gold re-rolls with it, the
    // Skyrim behavior). VendorState is a reflected component so the save
    // layer carries the clock (a scene map would reset on re-enter = a
    // free-restock exploit).
    constexpr f64 kRestockHours = 24.0;
    const f64 nowHours = gameClock.gameHours();
    if (!containerEntity.has<gameplay::VendorState>()) {
        containerEntity.set<gameplay::VendorState>({});
    }
    auto& vendorState = containerEntity.get_mut<gameplay::VendorState>();
    if (vendorState.lastRestockHours <= 0.0f) {
        // First open: stamp the clock, the spawn loadout IS the stock.
        vendorState.lastRestockHours = static_cast<f32>(nowHours);
    } else if (vendorFormId.isValid() &&
               nowHours - vendorState.lastRestockHours > kRestockHours) {
        auto& stock = containerEntity.get_mut<gameplay::Inventory>();
        stock.items.clear();
        gameplay::applyLoadout(forms, vendorFormId, stock, lootRng);
        vendorState.lastRestockHours = static_cast<f32>(nowHours);
        LOG_INFO("Vendor restocked ({}h game time)", nowHours);
    }
    pushItemModels();
    screenStack.show("barter");
}

void LandscapeScene::barterTrade(const core::Guid& item, bool playerBuys) {
    if (!barterMode || !goldForm || !containerEntity.is_alive() ||
        !playerEntity.is_alive()) {
        return;
    }
    // The unit value comes from the view row (already resolved per kind).
    const InventoryView& side = playerBuys ? lootView : invView;
    const InventoryView::Row* row = nullptr;
    for (const InventoryView::Row& candidate : side.rows()) {
        if (candidate.id == item) {
            row = &candidate;
            break;
        }
    }
    if (!row) {
        return;
    }
    auto& bag = playerEntity.get_mut<gameplay::Inventory>();
    auto& stock = containerEntity.get_mut<gameplay::Inventory>();
    if (playerBuys) {
        const i32 price = barterPrice(row->value, vendorBuyMult);
        barterBuy(bag, stock, item, price, goldForm->id);
    } else {
        const i32 price = barterPrice(row->value, vendorSellMult);
        barterSell(bag, stock, item, price, goldForm->id);
    }
}

void LandscapeScene::pushItemModels() {
    if (!uiCreated) {
        return;
    }
    static const gameplay::Inventory kEmptyBag;
    const gameplay::Inventory* bag = &kEmptyBag;
    const gameplay::Equipment* equipment = nullptr;
    if (playerEntity.is_alive()) {
        if (playerEntity.has<gameplay::Inventory>()) {
            bag = &playerEntity.get<gameplay::Inventory>();
        }
        if (playerEntity.has<gameplay::Equipment>()) {
            equipment = &playerEntity.get<gameplay::Equipment>();
        }
    }
    invView.build(forms, *bag, equipment);

    // In barter mode the value column shows the PRICE at the relevant
    // multiplier (sell on the player side, buy on the vendor side).
    const auto pushRows = [this](const InventoryView& view,
                                 const str& model, f32 priceMult) {
        vector<::ui::UiRow> rows;
        rows.reserve(view.rows().size());
        char buffer[32];
        for (const InventoryView::Row& row : view.rows()) {
            ::ui::UiRow out;
            out.id = row.id.toString();
            out.c0 = row.count > 1
                         ? row.name + "  x" + std::to_string(row.count)
                         : row.name;
            std::snprintf(buffer, sizeof(buffer), "%.1f", row.weight);
            out.c1 = buffer;
            out.c2 = std::to_string(
                priceMult > 0.0f ? barterPrice(row.value, priceMult)
                                 : row.value);
            out.c3 = row.power > 0.0f
                         ? std::to_string(
                               static_cast<i32>(row.power + 0.5f))
                         : str { "-" };
            out.selected = row.id == view.selected();
            out.tag = row.equipped ? "equipped" : "";
            rows.push_back(std::move(out));
        }
        uiSystem.setRows(model, std::move(rows));
    };
    pushRows(invView, "inventory", barterMode ? vendorSellMult : 0.0f);

    // C3: weight / max + the encumbrance category.
    char footer[96];
    f32 maxEncumbrance = 0.0f;
    if (playerEntity.is_alive()) {
        maxEncumbrance = gameplay::currentValueOf(
            playerEntity.get<gameplay::AbilitySystem>(),
            gameplay::attr("maxEncumbrance"));
    }
    std::snprintf(footer, sizeof(footer),
                  "Carried weight  %.1f / %.0f  (%s)", invView.totalWeight(),
                  maxEncumbrance,
                  gameplay::encumbranceLabel(gameplay::encumbranceCategory(
                      invView.totalWeight(), maxEncumbrance)));
    uiSystem.setString("inventory", "weightText", footer);

    const InventoryView::Row* selected = invView.selectedRow();
    uiSystem.setBool("inventory", "hasSelection", selected != nullptr);
    if (selected) {
        uiSystem.setString("inventory", "detailName", selected->name);
        char info[96];
        std::snprintf(info, sizeof(info),
                      "Weight %.1f   Value %d%s%s", selected->weight,
                      selected->value,
                      selected->power > 0.0f ? "   Power " : "",
                      selected->power > 0.0f
                          ? std::to_string(
                                static_cast<i32>(selected->power + 0.5f))
                                .c_str()
                          : "");
        uiSystem.setString("inventory", "detailInfo", info);
        uiSystem.setBool("inventory", "selUsable", selected->usable);
        uiSystem.setString("inventory", "equipLabel",
                           selected->equipped ? "Unequip" : "Equip");
    }

    if (containerEntity.is_alive() &&
        containerEntity.has<gameplay::Inventory>()) {
        lootView.build(forms, containerEntity.get<gameplay::Inventory>(),
                       nullptr);
        if (barterMode) {
            pushRows(lootView, "barter", vendorBuyMult);
            if (goldForm) {
                const auto& stock =
                    containerEntity.get<gameplay::Inventory>();
                uiSystem.setString(
                    "barter", "vendorGold",
                    std::to_string(
                        gameplay::itemCount(stock, goldForm->id)));
                uiSystem.setString(
                    "inventory", "goldText",
                    std::to_string(gameplay::itemCount(*bag, goldForm->id)));
            }
        } else {
            pushRows(lootView, "container", 0.0f);
            uiSystem.setString("container", "title", "Loot");
        }
    }
}

void LandscapeScene::handleUiEvent(const str& model, const str& event,
                                   const vector<str>& args) {
    const auto argGuid = [&]() -> std::optional<core::Guid> {
        return args.empty() ? std::nullopt
                            : core::Guid::fromString(args[0]);
    };
    if (model == "inventory") {
        if (event == "tab" && !args.empty()) {
            using Category = InventoryView::Category;
            Category category = Category::All;
            if (args[0] == "weapons") {
                category = Category::Weapons;
            } else if (args[0] == "armor") {
                category = Category::Armor;
            } else if (args[0] == "consumables") {
                category = Category::Consumables;
            } else if (args[0] == "misc") {
                category = Category::Misc;
            }
            invView.setCategory(category);
        } else if (event == "sortCol" && !args.empty()) {
            using Column = InventoryView::Column;
            Column column = Column::Name;
            if (args[0] == "weight") {
                column = Column::Weight;
            } else if (args[0] == "value") {
                column = Column::Value;
            } else if (args[0] == "power") {
                column = Column::Power;
            }
            invView.sortBy(column);
        } else if (event == "pick") {
            if (const auto id = argGuid()) {
                if (barterMode) {
                    barterTrade(*id, /*playerBuys=*/false); // sell
                } else if (containerEntity.is_alive()) {
                    transferItem(*id, /*fromContainer=*/false);
                } else {
                    invView.select(*id);
                }
            }
        } else if (event == "equipAction") {
            toggleEquip(invView.selected());
        } else if (event == "useAction") {
            useConsumable(invView.selected());
        }
        pushItemModels();
    } else if (model == "container") {
        if (event == "pickLoot") {
            if (const auto id = argGuid()) {
                transferItem(*id, /*fromContainer=*/true);
            }
        } else if (event == "takeAll" && containerEntity.is_alive() &&
                   playerEntity.is_alive()) {
            auto& loot = containerEntity.get_mut<gameplay::Inventory>();
            auto& bag = playerEntity.get_mut<gameplay::Inventory>();
            for (const gameplay::ItemStack& stack : loot.items) {
                if (stack.count > 0) {
                    gameplay::addItem(bag, stack.item, stack.count);
                }
            }
            loot.items.clear();
        }
        pushItemModels();
    } else if (model == "barter") {
        if (event == "pickBuy") {
            if (const auto id = argGuid()) {
                barterTrade(*id, /*playerBuys=*/true);
            }
        }
        pushItemModels();
    } else if (model == "menu") {
        if (event == "menuAction" && !args.empty()) {
            handleMenuAction(args[0]);
        }
    } else if (model == "saves") {
        if (event == "loadSlot" && !args.empty()) {
            screenStack.closeAll();
            requestLoad(args[0]);
        } else if (event == "loadCancel") {
            screenStack.closeTop();
        }
    } else if (model == "journal") {
        if (event == "journalClose") {
            screenStack.closeTop();
        }
    } else if (model == "dialogue") {
        if (event == "choose" && !args.empty() && dialogueRunner) {
            if (args[0] == "leave") {
                dialogueRunner->end();
            } else if (const auto id = core::Guid::fromString(args[0])) {
                for (const quest::DialogueNodeForm* option :
                     dialogueOptions) {
                    if (option->id == *id) {
                        dialogueRunner->select(*option);
                        break;
                    }
                }
            }
            pushDialogueModel(); // closes the screen when it ended
        }
    }
}

void LandscapeScene::toggleEquip(const core::Guid& id) {
    if (!id.isValid() || !playerEntity.is_alive() ||
        !playerEntity.has<gameplay::Equipment>()) {
        return;
    }
    auto& equipment = playerEntity.get_mut<gameplay::Equipment>();
    const data::FormHandle handle = forms.handleOf(id);
    const reflect::TypeInfo* type = forms.typeOf(handle);
    if (!type) {
        return;
    }
    if (type->isA(data::WeaponForm::staticTypeInfo().id)) {
        equipment.weapon = equipment.weapon == id ? core::Guid {} : id;
    } else if (type->isA(data::ArmorForm::staticTypeInfo().id)) {
        const auto* armor =
            static_cast<const data::ArmorForm*>(forms.get(handle));
        core::Guid* slot = nullptr;
        if (armor->slot == "head") {
            slot = &equipment.head;
        } else if (armor->slot == "torso") {
            slot = &equipment.torso;
        } else if (armor->slot == "arms") {
            slot = &equipment.arms;
        } else if (armor->slot == "legs") {
            slot = &equipment.legs;
        }
        if (slot) {
            *slot = *slot == id ? core::Guid {} : id;
        }
    }
}

void LandscapeScene::useConsumable(const core::Guid& id) {
    if (!id.isValid() || !playerEntity.is_alive()) {
        return;
    }
    const auto* consumable = forms.find<data::ConsumableForm>(id);
    if (!consumable || !playerEntity.has<gameplay::Inventory>()) {
        return;
    }
    auto& bag = playerEntity.get_mut<gameplay::Inventory>();
    if (!gameplay::removeItem(bag, id, 1)) {
        return;
    }
    // Survival needs are component fields (the sleep() precedent);
    // attribute changes still go through effects only (§2.9).
    if (playerEntity.has<gameplay::Survival>()) {
        auto& survival = playerEntity.get_mut<gameplay::Survival>();
        survival.hunger = glm::min(100.0f, survival.hunger +
                                               consumable->restoreHunger);
        survival.thirst = glm::min(100.0f, survival.thirst +
                                               consumable->restoreThirst);
    }
    if (consumable->effect.isValid()) {
        if (const auto* effect =
                forms.find<gameplay::EffectForm>(consumable->effect)) {
            gameplay::applyEffect(
                playerEntity.get_mut<gameplay::AttributeSet>(),
                playerEntity.get_mut<gameplay::AbilitySystem>(), *effect,
                gameTags);
        }
    }
    LOG_INFO("Used: {}", consumable->editorId);
}

// --- Chantier 4 B6: menus -----------------------------------------------------------

void LandscapeScene::updateMenuClockLine() {
    const f64 hours = std::fmod(gameClock.gameHours(), 24.0);
    const i32 hh = static_cast<i32>(hours);
    const i32 mm = static_cast<i32>((hours - hh) * 60.0);
    const i32 day = static_cast<i32>(gameClock.gameDays()) + 1;
    char line[48];
    std::snprintf(line, sizeof(line), "Day %d — %02d:%02d", day, hh, mm);
    uiSystem.setString("menu", "clockLine", line);
}

void LandscapeScene::performWait(f32 hours) {
    // Waiting passes game time and decays the needs, but restores nothing
    // — that's what beds are for (gameplay::sleep, B7-lite chantier 3).
    const f64 gameDt = static_cast<f64>(hours) * 3600.0;
    gameClock.gameSeconds += gameDt;
    if (playerEntity.is_alive()) {
        if (playerEntity.has<gameplay::Survival>()) {
            gameplay::tickSurvival(
                playerEntity.get_mut<gameplay::Survival>(), gameDt,
                statsTuning);
        }
        if (playerEntity.has<gameplay::CombatState>()) {
            gameplay::accrueRest(
                playerEntity.get_mut<gameplay::CombatState>(), gameDt);
        }
    }
    updateMenuClockLine();
    LOG_INFO("B6: waited {} h -> game time {:.2f} h", hours,
             std::fmod(gameClock.gameHours(), 24.0));
}

void LandscapeScene::handleMenuAction(const str& action) {
    if (action == "resume" || action == "cancel") {
        screenStack.closeTop();
    } else if (action == "save") {
        // Timestamped manual slot; F5 owns "quick".
        char slot[32];
        const std::time_t now = std::time(nullptr);
        std::tm local {};
        if (localtime_s(&local, &now) == 0) {
            std::strftime(slot, sizeof(slot), "save_%Y%m%d_%H%M%S", &local);
        } else {
            std::snprintf(slot, sizeof(slot), "save_manual");
        }
        performSave(slot);
        screenStack.closeTop();
    } else if (action == "loadmenu") {
        vector<::ui::UiRow> rows;
        for (const SaveSlotInfo& info : listSaveSlots()) {
            ::ui::UiRow row;
            row.id = info.name;
            row.c0 = info.name;
            row.c1 = info.timestamp;
            rows.push_back(std::move(row));
        }
        uiSystem.setBool("saves", "empty", rows.empty());
        uiSystem.setRows("saves", std::move(rows));
        screenStack.show("saves");
    } else if (action == "wait") {
        updateMenuClockLine();
        screenStack.show("wait");
    } else if (action == "wait1" || action == "wait4" ||
               action == "wait8") {
        performWait(action == "wait1" ? 1.0f
                                      : action == "wait4" ? 4.0f : 8.0f);
        screenStack.closeTop();
    } else if (action == "play") {
        screenStack.closeAll();
        if (!playMode) {
            enterPlayMode();
        }
    } else if (action == "mainmenu") {
        if (playMode) {
            exitPlayMode();
        }
        screenStack.closeAll();
        updateMenuClockLine();
        screenStack.show("mainmenu");
    } else if (action == "quit") {
        engine->requestQuit();
    }
}

// --- Chantier 4 B7: console + nameplates --------------------------------------------

void LandscapeScene::createConsole() {
    consoleSession = std::make_unique<data::EditSession>(forms, formTypes);
    consoleVm = std::make_unique<script::Vm>();
    console = std::make_unique<ConsolePanel>(*consoleSession, forms,
                                             formTypes, *consoleVm);
    // World commands (the H2 note: registered by the scene that owns a
    // world; reflection stays the backbone for get/set).
    console->addCommand("spawn", [this](const str& args) -> str {
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
        const Vec3 origin = playMode && player ? player->position()
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
        snapCellEntities();
        refreshNpcs(engine->getDevice()); // actors need their rig/brain
        return "spawned " + args + " (transient — not saved)";
    });
    console->addCommand("tp", [this](const str& args) -> str {
        std::istringstream in { args };
        f32 x = 0.0f, z = 0.0f;
        if (!(in >> x >> z)) {
            return "usage: tp <x> <z>";
        }
        const f32 y = render::terrain::height(terrain.params, x, z) + 0.5f;
        if (playMode && player) {
            player = std::make_unique<phys::CharacterBody>(
                *physics, 0.3f, 1.8f, Vec3 { x, y, z });
            playerVelocity = Vec3 { 0.0f };
        } else {
            flyCamera.camera.position = { x, y + 1.7f, z };
        }
        char out[64];
        std::snprintf(out, sizeof(out), "teleported to %.0f %.0f", x, z);
        return out;
    });
    console->addCommand("tgm", [this](const str&) -> str {
        godMode = !godMode;
        return godMode ? "god mode ON" : "god mode OFF";
    });
    console->addCommand("save", [this](const str& args) -> str {
        performSave(args.empty() ? "quick" : args);
        return "saved '" + (args.empty() ? str { "quick" } : args) + "'";
    });
    console->addCommand("load", [this](const str& args) -> str {
        const str slot = args.empty() ? "quick" : args;
        if (!std::filesystem::exists(savePath(slot))) {
            return "no save named '" + slot + "'";
        }
        requestLoad(slot);
        return "loading '" + slot + "'...";
    });
    console->addCommand("startquest", [this](const str& args) -> str {
        const auto* quest =
            data::findByEditorId<quest::QuestForm>(forms, args);
        if (!quest) {
            return "no quest named '" + args + "'";
        }
        if (questLog.quests.contains(quest->id)) {
            return "'" + args + "' already in the log";
        }
        quest::beginQuest(questLog, forms, quest->id);
        syncQuestTags();
        return "quest '" + args + "' started";
    });
    console->addCommand("queststate", [this](const str&) -> str {
        if (questLog.quests.empty()) {
            return "quest log empty";
        }
        str out;
        for (const auto& [id, progress] : questLog.quests) {
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
    console->addCommand("settime", [this](const str& args) -> str {
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

void LandscapeScene::updateNameplates() {
    vector<::ui::UiRow> plates;
    if (playMode && player && uiCreated) {
        const f32 width = static_cast<f32>(engine->getWindow().width());
        const f32 height = static_cast<f32>(engine->getWindow().height());
        const Mat4 viewProj =
            flyCamera.camera.viewProj(height > 0.0f ? width / height : 1.0f);
        for (const auto& npcPtr : npcs) {
            const Npc& npc = *npcPtr;
            if (npc.dead || !npc.entity.is_alive() ||
                !npc.entity.has<gameplay::AbilitySystem>()) {
                continue;
            }
            const Vec3 position =
                npc.entity.get<world::Transform>().position;
            const Vec3 to = position - player->position();
            if (glm::dot(to, to) > 15.0f * 15.0f) {
                continue;
            }
            const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
            const f32 health =
                gameplay::currentValueOf(sys, gameplay::attr("health"));
            const f32 maxHealth =
                gameplay::currentValueOf(sys, gameplay::attr("maxHealth"));
            // Nameplates single out threats and the wounded (SkyUI-style
            // restraint: a healthy villager stays unlabelled).
            if (!npc.hostile && health >= maxHealth - 0.5f) {
                continue;
            }
            const Vec4 clip =
                viewProj * Vec4 { position + Vec3 { 0.0f, 2.15f, 0.0f },
                                  1.0f };
            if (clip.w <= 0.1f) {
                continue; // behind the camera
            }
            const f32 px = (clip.x / clip.w * 0.5f + 0.5f) * width;
            const f32 py = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * height;
            ::ui::UiRow plate;
            plate.id = std::to_string(npc.entity.id());
            str name = "?";
            if (npc.entity.has<world::RefId>()) {
                const auto& ref = npc.entity.get<world::RefId>();
                if (const reflect::TypeInfo* type = forms.typeOf(ref.base);
                    type &&
                    type->isA(data::ActorForm::staticTypeInfo().id)) {
                    name = static_cast<const data::ActorForm*>(
                               forms.get(ref.base))
                               ->displayName;
                }
            }
            plate.c0 = name;
            plate.c1 = std::to_string(static_cast<i32>(glm::clamp(
                100.0f * health / glm::max(maxHealth, 1.0f), 0.0f,
                100.0f)));
            plate.c2 = std::to_string(static_cast<i32>(px - 60.0f));
            plate.c3 = std::to_string(static_cast<i32>(py));
            plate.tag = npc.hostile ? "hostile" : "";
            plates.push_back(std::move(plate));
        }
    }
    uiSystem.setRows("hud", std::move(plates));
}

// --- Chantier 6 A2: quests ----------------------------------------------------------

void LandscapeScene::syncQuestTags() {
    if (!easternQuest || !playerEntity.is_alive() ||
        !playerEntity.has<gameplay::AbilitySystem>()) {
        return;
    }
    auto& system = playerEntity.get_mut<gameplay::AbilitySystem>();
    const auto syncTag = [&](const char* name, bool want) {
        const auto tag = gameTags.find(name);
        if (!tag) {
            return;
        }
        const bool have = system.tags.has(*tag);
        if (want && !have) {
            system.tags.add(*tag, gameTags);
        } else if (!want && have) {
            system.tags.remove(*tag, gameTags);
        }
    };
    const bool active = quest::isActive(questLog, easternQuest->id);
    const auto* reportState = data::findByEditorId<quest::QuestStateForm>(
        forms, "EasternMenaceReport");
    const bool ready =
        active && reportState &&
        quest::questState(questLog, easternQuest->id) == reportState->id;
    const bool done = quest::questStatus(questLog, easternQuest->id) ==
                      quest::QuestStatus::Succeeded;
    syncTag("Quest.EasternMenace.Active", active);
    syncTag("Quest.EasternMenace.Ready", ready);
    syncTag("Quest.EasternMenace.Done", done);
}

void LandscapeScene::handleQuestEvent(const gameplay::Event& event) {
    if (!easternQuest) {
        return;
    }
    const core::Guid stateBefore =
        quest::questState(questLog, easternQuest->id);
    quest::onQuestEvent(questLog, forms, event, gameTags);
    syncQuestTags();
    const bool succeeded = quest::questStatus(questLog, easternQuest->id) ==
                           quest::QuestStatus::Succeeded;
    if (succeeded && stateBefore.isValid() &&
        quest::questState(questLog, easternQuest->id) != stateBefore) {
        // The turn-in option fires exactly once (its gate tag drops with
        // the transition) — the reward lands here, no flag to persist.
        if (goldForm && playerEntity.is_alive() &&
            playerEntity.has<gameplay::Inventory>()) {
            gameplay::addItem(playerEntity.get_mut<gameplay::Inventory>(),
                              goldForm->id, 50);
        }
        talkLine = "Quete accomplie : La menace de l'est (+50 or).";
        talkTimer = 5.0f;
    } else if (quest::questState(questLog, easternQuest->id) !=
               stateBefore) {
        talkLine = "Journal mis a jour (J).";
        talkTimer = 4.0f;
    }
}

void LandscapeScene::pushJournalModel() {
    if (!uiCreated) {
        return;
    }
    // Deterministic listing (§8): quests sorted by guid.
    vector<core::Guid> questIds;
    questIds.reserve(questLog.quests.size());
    for (const auto& [id, progress] : questLog.quests) {
        questIds.push_back(id);
    }
    std::sort(questIds.begin(), questIds.end());

    vector<::ui::UiRow> rows;
    for (const core::Guid& questId : questIds) {
        const quest::QuestProgress& progress = questLog.quests.at(questId);
        const auto* questForm = forms.find<quest::QuestForm>(questId);
        if (!questForm) {
            continue; // a mod removed the quest — skip, never fatal (§5)
        }
        ::ui::UiRow header;
        header.id = questId.toString();
        header.c0 = questForm->displayName;
        if (progress.status == quest::QuestStatus::Succeeded) {
            header.c1 = "Accomplie";
            header.tag = "done";
        } else if (progress.status == quest::QuestStatus::Failed) {
            header.c1 = "Echouee";
            header.tag = "done";
        }
        rows.push_back(std::move(header));
        if (progress.status != quest::QuestStatus::Active) {
            continue;
        }
        // Objectives = the tasks of the current state's branches.
        data::forEach<quest::QuestBranchForm>(
            forms, [&](const quest::QuestBranchForm& branch) {
                if (branch.state != progress.currentState) {
                    return;
                }
                data::forEach<quest::QuestTaskForm>(
                    forms, [&](const quest::QuestTaskForm& task) {
                        if (task.branch != branch.id) {
                            return;
                        }
                        ::ui::UiRow row;
                        row.id = task.id.toString();
                        row.c0 = task.displayName;
                        if (task.required > 1) {
                            row.c2 =
                                std::to_string(quest::taskProgress(
                                    questLog, questId, task.id)) +
                                " / " + std::to_string(task.required);
                        }
                        row.tag = "task";
                        rows.push_back(std::move(row));
                    });
            });
    }
    uiSystem.setBool("journal", "empty", rows.empty());
    uiSystem.setRows("journal", std::move(rows));
}

// --- Chantier 4 B4: dialogue --------------------------------------------------------

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

void LandscapeScene::openDialogue(const core::Guid& dialogueId) {
    if (!uiCreated) {
        return;
    }
    if (!dialogueRunner) {
        dialogueRunner =
            std::make_unique<quest::DialogueRunner>(forms, eventBus);
    }
    if (!dialogueRunner->start(dialogueId)) {
        LOG_WARN("B4: dialogue {} failed to start", dialogueId.toString());
        return;
    }
    if (const auto* dialogue =
            forms.find<quest::DialogueForm>(dialogueId)) {
        uiSystem.setString("dialogue", "npcName", dialogue->displayName);
    }
    pushDialogueModel();
    screenStack.show("dialogue");
}

void LandscapeScene::pushDialogueModel() {
    if (!dialogueRunner || !dialogueRunner->active()) {
        screenStack.close("dialogue");
        dialogueOptions.clear();
        return;
    }
    const quest::DialogueNodeForm* line = dialogueRunner->currentLine();
    uiSystem.setString("dialogue", "npcLine", line ? line->text : str {});
    dialogueOptions = dialogueRunner->options(makeEvalContext());
    vector<::ui::UiRow> rows;
    u32 index = 1;
    for (const quest::DialogueNodeForm* option : dialogueOptions) {
        ::ui::UiRow row;
        row.id = option->id.toString();
        row.c0 = std::to_string(index++) + ".  " + option->text;
        rows.push_back(std::move(row));
    }
    ::ui::UiRow leave;
    leave.id = "leave";
    leave.c0 = std::to_string(index) + ".  (Leave)";
    leave.tag = "leave";
    rows.push_back(std::move(leave));
    uiSystem.setRows("dialogue", std::move(rows));
}

void LandscapeScene::transferItem(const core::Guid& id,
                                  bool fromContainer) {
    if (!id.isValid() || !containerEntity.is_alive() ||
        !playerEntity.is_alive()) {
        return;
    }
    if (!containerEntity.has<gameplay::Inventory>() ||
        !playerEntity.has<gameplay::Inventory>()) {
        return;
    }
    auto& loot = containerEntity.get_mut<gameplay::Inventory>();
    auto& bag = playerEntity.get_mut<gameplay::Inventory>();
    auto& source = fromContainer ? loot : bag;
    auto& target = fromContainer ? bag : loot;
    if (gameplay::removeItem(source, id, 1)) {
        gameplay::addItem(target, id, 1);
    }
}

// Chantier 3 B6: first-person melee — LMB swings the equipped weapon at
// the nearest living NPC in reach and roughly in front. Damage flows
// through the SAME GAS pipeline as the 2D arena (§2.9: no hand-rolled
// numbers). v1 has no swing animation (no visible body) — the cooldown
// and the hit feedback carry the feel until the FX/audio brick.
void LandscapeScene::tryPlayerAttack() {
    if (!playerEntity.is_alive() || !player) {
        return;
    }
    // Chantier 4 B3: the swing uses the EQUIPPED weapon (inventory screen
    // can swap/unequip it); bare hands don't attack in v1.
    const data::WeaponForm* weapon = playerWeapon;
    if (playerEntity.has<gameplay::Equipment>()) {
        const auto& equipment = playerEntity.get<gameplay::Equipment>();
        weapon = equipment.weapon.isValid()
                     ? forms.find<data::WeaponForm>(equipment.weapon)
                     : nullptr;
    }
    if (!weapon) {
        LOG_INFO("Swing: no weapon equipped");
        return;
    }
    playerAttackCooldown = 0.7f;
    const Vec3 eye = player->position() + Vec3 { 0.0f, 1.7f, 0.0f };
    const Vec3 forward = flyCamera.camera.forward();
    Npc* best = nullptr;
    f32 bestScore = 0.45f;
    for (auto& npcPtr : npcs) {
        Npc& npc = *npcPtr;
        if (npc.dead || !npc.entity.is_alive()) {
            continue;
        }
        const Vec3 position =
            npc.entity.get<world::Transform>().position;
        const Vec3 to = position + Vec3 { 0.0f, 1.1f, 0.0f } - eye;
        const f32 distance = glm::length(to);
        if (distance > 2.4f || distance < 1e-3f) {
            continue;
        }
        const f32 facing = glm::dot(to / distance, forward);
        if (facing > bestScore) {
            bestScore = facing;
            best = &npc;
        }
    }
    if (!best) {
        LOG_INFO("Swing: nothing in reach");
        return;
    }
    gameplay::StatBlock block {
        best->entity.get_mut<gameplay::CoreAttributes>(),
        best->entity.get_mut<gameplay::AttributeSet>(),
        best->entity.get_mut<gameplay::AbilitySystem>(),
        best->entity.get_mut<gameplay::CombatState>()
    };
    const auto& playerSys = playerEntity.get<gameplay::AbilitySystem>();
    gameplay::DamageEvent event =
        gameplay::weaponDamageEvent(*weapon, playerSys);
    // C1: a target in its critical window eats the critical execution.
    if (const auto weakness = gameTags.find("State.CriticalWeakness")) {
        event.critical = block.system.tags.has(*weakness);
    }
    const gameplay::DamageResult result = gameplay::applyDamage(
        block, event, gameTags, derivedStats, nullptr, statsTuning);
    LOG_INFO("You hit for {:.0f} damage{}{} (target health {:.0f})",
             result.healthDamage, event.critical ? " — CRITICAL!" : "",
             result.staggered ? " — staggered!" : "",
             gameplay::currentValueOf(
                 best->entity.get<gameplay::AbilitySystem>(),
                 gameplay::attr("health")));

    // D2 — crime v1: assaulting a peaceful NPC in front of a witness.
    // Witnesses = the victim (if still alive) or any living NPC within
    // earshot with a clear line to the player (the B5 raycast idiom).
    if (!best->hostile) {
        constexpr f32 kBountyAssault = 40.0f;
        constexpr f32 kWitnessRange = 20.0f;
        bool witnessed = !best->dead && best->entity.is_alive();
        for (const auto& witnessPtr : npcs) {
            if (witnessed) {
                break;
            }
            const Npc& witness = *witnessPtr;
            if (&witness == best || witness.dead ||
                !witness.entity.is_alive()) {
                continue;
            }
            const Vec3 witnessEye =
                witness.entity.get<world::Transform>().position +
                Vec3 { 0.0f, 1.5f, 0.0f };
            const Vec3 toPlayer = eye - witnessEye;
            const f32 sight = glm::length(toPlayer);
            if (sight > kWitnessRange || sight < 1e-3f) {
                continue;
            }
            const phys::RayHit hit =
                physics->rayCast(witnessEye, toPlayer / sight, sight);
            witnessed = !(hit.hit && hit.distance < sight - 0.6f);
        }
        if (witnessed && playerEntity.is_alive()) {
            auto& bounty = playerEntity.get_mut<gameplay::Bounty>();
            bounty.bounty += kBountyAssault;
            syncWantedTag();
            talkLine = "Crime observe ! Prime : " +
                       std::to_string(static_cast<i32>(bounty.bounty)) +
                       " pieces d'or.";
            talkTimer = 4.0f;
            LOG_INFO("Crime witnessed — bounty {:.0f}", bounty.bounty);
        }
    }
}

void LandscapeScene::syncWantedTag() {
    if (!playerEntity.is_alive()) {
        return;
    }
    const auto tag = gameTags.find("Crime.Wanted");
    if (!tag) {
        return;
    }
    const bool wanted = playerEntity.has<gameplay::Bounty>() &&
                        playerEntity.get<gameplay::Bounty>().bounty > 0.0f;
    auto& system = playerEntity.get_mut<gameplay::AbilitySystem>();
    if (wanted) {
        system.tags.add(*tag, gameTags);
    } else {
        system.tags.remove(*tag, gameTags);
    }
}

void LandscapeScene::updatePlayer(f32 dt) {
    platform::Input& input = engine->getInput();
    if (fadeDirection != 0) {
        return; // frozen during door transitions
    }
    // B6: melee swing on LMB (the mouse is captured in Play — ImGui
    // never owns it here).
    playerAttackCooldown -= dt;
    if (playerAttackCooldown <= 0.0f &&
        input.mousePressed(platform::MouseButton::Left)) {
        tryPlayerAttack();
    }

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
    // C3: overencumbered = no sprint, no jump (STATS.md §3 Utility).
    const bool overencumbered =
        playerEncumbrance == gameplay::EncumbranceCategory::Overencumbered;
    const bool sprinting = moving && input.isDown(platform::Key::Shift) &&
                           energy > 1.0f && !overencumbered;
    const f32 targetSpeed = sprinting ? jog * kSprintMult : jog;
    const Vec3 target =
        moving ? glm::normalize(wish) * targetSpeed : Vec3 { 0.0f };
    // Exponential smoothing toward the target: snappy, never binary.
    playerVelocity += (target - playerVelocity) *
                      (1.0f - std::exp(-accelRate * dt));
    if (input.wasPressed(platform::Key::Space) && !overencumbered) {
        // C3: jump velocity from the jumpPower stat (default sheet 104
        // → the previous hand-tuned 5.0 m/s via kJumpScale3D).
        f32 jump = jumpSpeed; // fallback without a Player actor
        if (playerEntity.is_alive()) {
            jump = gameplay::currentValueOf(
                       playerEntity.get<gameplay::AbilitySystem>(),
                       gameplay::attr("jumpPower")) *
                   kJumpScale3D;
        }
        player->jump(jump);
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

// Chantier 2 B2: static colliders follow the spawned statics. Runs every
// frame (cheap: map lookups + a small query) because meshes turn resident
// asynchronously — a newcomer gets its body the frame its CPU data lands.
void LandscapeScene::updateStaticColliders() {
    if (!physics || !meshCache) {
        return;
    }
    for (auto it = staticColliders.begin(); it != staticColliders.end();) {
        if (!world.handle().is_alive(static_cast<flecs::entity_t>(it->first))) {
            physics->removeBody(it->second);
            it = staticColliders.erase(it);
        } else {
            ++it;
        }
    }
    // Cook budget: a Jolt MeshShape cook is main-thread and expensive —
    // a cell's worth of kit meshes turning resident in one frame used to
    // cost 100+ ms (the frame-probe smoking gun). Two per frame in
    // normal play, UNCAPPED while the travel fade holds the screen black
    // (the fade exists to hide exactly this — and a starved budget once
    // dropped the player through a not-yet-solid floor). NEAREST FIRST,
    // so the ground underfoot is always the first body to exist.
    u32 cookBudget =
        (fadeDirection != 0 || fadeAlpha > 0.0f) ? 4096 : 2;
    const Vec3 cookFocus = playMode && player ? player->position()
                                              : flyCamera.camera.position;
    struct CookCandidate {
        f32 distSq;
        u64 id;
        const MeshCache::CpuMesh* cpu;
        Vec3 position;
        Quat rotation;
        Vec3 scale;
    };
    vector<CookCandidate> cooks;
    colliderQuery.each(
        [&](flecs::entity e, const world::Transform& transform,
            const world::RefId& ref, const world::MeshRender& mesh) {
            const u64 id = e.id();
            if (staticColliders.contains(id) || nonCollidable.contains(id)) {
                return;
            }
            // `collides` read through reflection: any base form declaring
            // it opts in (StaticForm today, DoorForm...). The negative
            // verdict is cached — reflection must not run per frame.
            const data::Form* base = forms.get(ref.base);
            const reflect::TypeInfo* type = forms.typeOf(ref.base);
            if (!base || !type) {
                nonCollidable.insert(id);
                return;
            }
            const reflect::FieldInfo* field = type->findField("collides");
            if (!field || field->kind != reflect::FieldKind::Bool ||
                !std::get<bool>(field->get(base))) {
                nonCollidable.insert(id);
                return;
            }
            const MeshCache::CpuMesh* cpu = meshCache->cpuMesh(mesh.model);
            if (!cpu) {
                return; // still streaming — retried next frame
            }
            const Vec3 d = transform.position - cookFocus;
            cooks.push_back({ glm::dot(d, d), id, cpu, transform.position,
                              transform.rotation, transform.scale });
        });
    std::sort(cooks.begin(), cooks.end(),
              [](const CookCandidate& a, const CookCandidate& b) {
                  return a.distSq < b.distSq;
              });
    for (const CookCandidate& cook : cooks) {
        if (cookBudget == 0) {
            break;
        }
        const phys::BodyId body = physics->addStaticMesh(
            cook.cpu->positions.data(),
            static_cast<u32>(cook.cpu->positions.size()),
            cook.cpu->indices.data(),
            static_cast<u32>(cook.cpu->indices.size()), cook.position,
            cook.rotation, cook.scale);
        if (body != 0) {
            staticColliders.emplace(cook.id, body);
            --cookBudget;
        }
    }
}

// Idempotent ground snap (chantier 2 B1): world Y = terrain height at
// (x, z) + the reference's AUTHORED y (an offset above ground until the
// level editor writes real heights). Safe to re-run after every cell
// change; prefab-derived children (no base record) keep their expanded Y.
// Skipped for: interior cells (no terrain — authored y is absolute) and
// base forms with snapToGround = false (building modules on a pad).
void LandscapeScene::snapCellEntities() {
    // Entities changed (cell ring, travel, spawn): stale negative
    // collider verdicts go with them.
    nonCollidable.clear();
    if (editMode) {
        return; // the editor owns transforms while it is active
    }
    const auto skipsSnap = [&](const world::ReferenceForm& reference,
                               data::FormHandle baseHandle) {
        if (const auto* cell =
                forms.find<world::CellForm>(reference.cell);
            cell && cell->interior) {
            return true;
        }
        const data::Form* base = forms.get(baseHandle);
        const reflect::TypeInfo* type = forms.typeOf(baseHandle);
        if (base && type) {
            if (const reflect::FieldInfo* field =
                    type->findField("snapToGround");
                field && field->kind == reflect::FieldKind::Bool &&
                !std::get<bool>(field->get(base))) {
                return true;
            }
        }
        return false;
    };
    world.handle()
        .query<world::Transform, const world::RefId,
               const world::MeshRender>()
        .each([&](flecs::entity, world::Transform& transform,
                  const world::RefId& ref, const world::MeshRender&) {
            const auto* reference =
                forms.find<world::ReferenceForm>(ref.referenceId);
            if (!reference || skipsSnap(*reference, ref.base)) {
                return;
            }
            transform.position.y =
                render::terrain::height(terrain.params, transform.position.x,
                                        transform.position.z) +
                reference->position.y;
        });
    // Lights too (no MeshRender): a torch's authored y is its height
    // above the ground it stands on.
    world.handle()
        .query<world::Transform, const world::RefId,
               const world::LightSource>()
        .each([&](flecs::entity, world::Transform& transform,
                  const world::RefId& ref, const world::LightSource&) {
            const auto* reference =
                forms.find<world::ReferenceForm>(ref.referenceId);
            if (!reference || skipsSnap(*reference, ref.base)) {
                return;
            }
            transform.position.y =
                render::terrain::height(terrain.params, transform.position.x,
                                        transform.position.z) +
                reference->position.y;
        });
}

void LandscapeScene::destroyNpc(rhi::Device& device, Npc& npc) {
    npc.anim.reset(); // references npc.graph — release first
    if (npc.casterGroup.id != 0) {
        device.destroyBindGroup(npc.casterGroup);
    }
    device.destroyBindGroup(npc.group);
    device.destroyBuffer(npc.modelUbo);
    device.destroyBuffer(npc.paletteSsbo);
    device.destroyBuffer(npc.indices);
    device.destroyBuffer(npc.vertices);
}

void LandscapeScene::refreshNpcs(rhi::Device& device) {
    // Prune NPCs whose entity was unloaded with its cell.
    for (auto it = npcs.begin(); it != npcs.end();) {
        if (!(*it)->entity.is_alive()) {
            destroyNpc(device, **it);
            it = npcs.erase(it);
        } else {
            ++it;
        }
    }

    // Patrol points: every LOADED "patrol" marker, grounded, spawn order.
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

    // Adding a component inside .each() is a structural change on a LOCKED
    // table (flecs LOCKED_STORAGE assert) — collect here, apply after.
    vector<std::pair<ecs::Entity, core::Guid>> pendingLoadouts;
    world.handle()
        .query<world::Transform, const world::RefId>()
        .each([&](flecs::entity e, world::Transform& transform,
                  const world::RefId& ref) {
            ecs::Entity entity { e };
            if (!entity.has<world::ActorMarker>() ||
                entity == playerEntity) {
                return;
            }
            for (const auto& tracked : npcs) {
                if (tracked->entity == entity) {
                    return; // already built
                }
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
            // Chantier 3 B3/B6: the daily routine + the anim tag gates
            // (sitting from furniture use, dead from the GAS life state).
            npc->schedule = actor.schedule;
            // Chantier 6 A1: ActorTagForm children become REAL gameplay
            // tags on the actor's system (registerTag is idempotent and
            // auto-registers ancestors); the first Faction.* tag is what
            // quests/crime filter deaths by. Mutating the EXISTING
            // AbilitySystem component inside .each is safe (no table
            // move).
            data::childrenOf<gameplay::ActorTagForm>(
                forms, actor.id, [&](const gameplay::ActorTagForm& tagForm) {
                    const gameplay::GameplayTag tag =
                        gameTags.registerTag(tagForm.tag);
                    if (entity.has<gameplay::AbilitySystem>()) {
                        entity.get_mut<gameplay::AbilitySystem>().tags.add(
                            tag, gameTags);
                    }
                    if (!npc->factionTag.isValid() &&
                        tagForm.tag.starts_with("Faction.")) {
                        npc->factionTag = tag;
                    }
                    if (tagForm.tag == "Faction.Bandits") {
                        npc->hostile = true;
                    }
                    if (tagForm.tag == "Faction.VillageGuard") {
                        npc->guard = true; // D2: aggro only while Wanted
                    }
                });
            npc->anim->setTagCheck(
                [raw = npc.get()](std::string_view tag) {
                    if (tag == "State.Sitting") {
                        return raw->sitting;
                    }
                    if (tag == "State.Dead") {
                        return raw->dead;
                    }
                    return false;
                });
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
            // skipped them).
            transform.position.y = render::terrain::height(
                terrain.params, transform.position.x, transform.position.z);
            // Chantier 5 B3: stats + saved state / loadout run through
            // finalizeActorSpawn — deferred below: it adds components,
            // a table move on the locked iteration.
            pendingLoadouts.emplace_back(entity, actor.id);

            if (npcs.empty()) {
                characterSpot = transform.position;
            }
            npcs.push_back(std::move(npc));
            LOG_INFO("B6: NPC '{}' built from Forms",
                     static_cast<const data::ActorForm*>(
                         forms.get(ref.base))
                         ->editorId);
        });
    for (auto& [entity, actorId] : pendingLoadouts) {
        finalizeActorSpawn(entity, actorId);
    }
    // Chantier 6 A1: seed the death flag from the (possibly restored)
    // life state, so a corpse reloaded from a save or a cell re-entry
    // never fires a spurious OnDeath edge on its first tick.
    if (const auto deadTag = gameTags.find("State.Dead")) {
        for (auto& npcPtr : npcs) {
            if (npcPtr->entity.is_alive() &&
                npcPtr->entity.has<gameplay::AbilitySystem>()) {
                npcPtr->dead =
                    npcPtr->entity.get<gameplay::AbilitySystem>().tags.has(
                        *deadTag);
            }
        }
    }
}

// Chantier 3 B2: the navigator's obstacle set = the static colliders'
// world AABBs, inflated by the agent radius. Refreshed on cell changes.
void LandscapeScene::refreshNavObstacles() {
    if (!navigator || !meshCache) {
        return;
    }
    vector<world::TerrainNavigator::BlockingBox> boxes;
    world.handle()
        .query<const world::Transform, const world::MeshRender,
               const world::RefId>()
        .each([&](flecs::entity, const world::Transform& transform,
                  const world::MeshRender& mesh, const world::RefId& ref) {
            const data::Form* base = forms.get(ref.base);
            const reflect::TypeInfo* type = forms.typeOf(ref.base);
            if (!base || !type) {
                return;
            }
            const reflect::FieldInfo* field = type->findField("collides");
            if (!field || field->kind != reflect::FieldKind::Bool ||
                !std::get<bool>(field->get(base))) {
                return;
            }
            Vec3 lo { -0.5f }, hi { 0.5f };
            if (const MeshCache::CpuMesh* cpu =
                    meshCache->cpuMesh(mesh.model)) {
                lo = cpu->boundsMin;
                hi = cpu->boundsMax;
            }
            const Mat4 model =
                glm::translate(Mat4 { 1.0f }, transform.position) *
                glm::mat4_cast(transform.rotation) *
                glm::scale(Mat4 { 1.0f }, transform.scale);
            Vec3 wlo { 1e9f }, whi { -1e9f };
            for (u32 i = 0; i < 8; ++i) {
                const Vec3 corner { (i & 1) ? hi.x : lo.x,
                                    (i & 2) ? hi.y : lo.y,
                                    (i & 4) ? hi.z : lo.z };
                const Vec3 w = Vec3 { model * Vec4 { corner, 1.0f } };
                wlo = glm::min(wlo, w);
                whi = glm::max(whi, w);
            }
            constexpr f32 kAgentRadius = 0.4f;
            boxes.push_back({ wlo - Vec3 { kAgentRadius, 0.0f, kAgentRadius },
                              whi + Vec3 { kAgentRadius, 0.0f,
                                           kAgentRadius } });
        });
    navigator->setBlockingBoxes(std::move(boxes));
}

// Chantier 3 B3: re-evaluate the schedule every 10 game minutes; execute
// the active package (travel / wander / useFurniture / guard...).
void LandscapeScene::updateNpcSchedule(Npc& npc, f32 hourOfDay) {
    const i32 slot = static_cast<i32>(hourOfDay * 6.0f);
    if (slot == npc.lastEvaluatedSlot) {
        return;
    }
    npc.lastEvaluatedSlot = slot;
    const auto intent =
        gameplay::evaluateSchedule(forms, npc.schedule, hourOfDay);
    const gameplay::AiPackageForm* next =
        intent ? intent->package : nullptr;
    const core::Guid nextLocation = intent ? intent->location
                                           : core::Guid {};
    if (next != npc.activePackage || nextLocation != npc.activeLocation) {
        // Package change: stand up, drop the path, release furniture.
        npc.activePackage = next;
        npc.activeLocation = nextLocation;
        npc.intentReason = intent ? intent->reason : "(no schedule entry)";
        npc.path.clear();
        npc.pathIndex = 0;
        npc.sitting = false;
        if (npc.furnitureClaimed) {
            furnitureOccupancy.release(npc.entity.id());
            npc.furnitureClaimed = false;
        }
    }
}

bool LandscapeScene::moveNpcAlongPath(Npc& npc, f32 dt, f32 speedScale) {
    if (npc.pathIndex >= npc.path.size()) {
        return true;
    }
    auto& transform = npc.entity.get_mut<world::Transform>();
    const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
    const f32 walkSpeed =
        gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
        kSpeedScale3D * kNpcWalkFactor * speedScale;

    const Vec3 goal = npc.path[npc.pathIndex];
    Vec3 to = goal - transform.position;
    to.y = 0.0f;
    const f32 distance = glm::length(to);
    if (distance < 0.35f) {
        ++npc.pathIndex;
        return npc.pathIndex >= npc.path.size();
    }
    const Vec3 dir = to / distance;
    transform.position += dir * glm::min(walkSpeed * dt, distance);
    transform.position.y = render::terrain::height(
        terrain.params, transform.position.x, transform.position.z);
    const f32 goalYaw = std::atan2(dir.x, dir.z);
    f32 delta = goalYaw - npc.yaw;
    while (delta > glm::pi<f32>()) {
        delta -= glm::two_pi<f32>();
    }
    while (delta < -glm::pi<f32>()) {
        delta += glm::two_pi<f32>();
    }
    npc.yaw += delta * (1.0f - std::exp(-8.0f * dt));
    transform.rotation = glm::angleAxis(npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    npc.speed += (walkSpeed - npc.speed) * (1.0f - std::exp(-10.0f * dt));
    return false;
}

void LandscapeScene::updateNpcs(f32 dt) {
    const f32 hourOfDay =
        static_cast<f32>(std::fmod(gameClock.gameHours(), 24.0));
    const f64 gameDt =
        static_cast<f64>(dt) * static_cast<f64>(gameClock.timescale);
    const gameplay::CharacterTickContext tickCtx { derivedStats, gameTags,
                                                   statsTuning };
    const auto deadTag = gameTags.find("State.Dead");
    for (auto& npcPtr : npcs) {
        Npc& npc = *npcPtr;
        auto& transform = npc.entity.get_mut<world::Transform>();
        f32 idleDecay = 10.0f;

        // B6: NPCs run the full character pipeline too (effects, stagger,
        // life state) — that's where State.Dead comes from.
        gameplay::tickCharacter(npc.entity, dt, gameDt, tickCtx);
        const auto& npcSys = npc.entity.get<gameplay::AbilitySystem>();
        const bool wasDead = npc.dead;
        npc.dead = deadTag && npcSys.tags.has(*deadTag);
        // Chantier 6 A1: the live->dead EDGE is the gameplay event —
        // quests (kill tasks) and crime listen on the bus. Reload paths
        // never fire it: refreshNpcs seeds npc.dead from the tag.
        if (npc.dead && !wasDead) {
            eventBus.dispatch({ gameplay::eventKind("OnDeath"),
                                ecs::Entity {}, npc.entity,
                                npc.factionTag });
        }
        // (The corpse is lootable — its Inventory was rolled from the
        // LoadoutEntryForm children at build, chantier 4 B5.)
        if (npc.dead) {
            // The death transition (anim graph, State.Dead gate) plays;
            // the body stays. Despawn: a later slice.
            npc.sitting = false;
            npc.path.clear();
            npc.speed = 0.0f;
            npc.anim->setParam("speed", 0.0f);
            npc.anim->update(dt, 0.0f);
            anim::bindPose(npc.rig->skeleton, npc.pose);
            npc.anim->evaluate(npc.pose);
            anim::skinMatrices(npc.rig->skeleton, npc.pose, npc.palette);
            continue;
        }

        // B5: hostile actors hunt the player on sight (distance + a clear
        // line — the perception cone can refine later). D2: a guard turns
        // hostile while the player carries a bounty (tag-based — the
        // relations table stays a later pass).
        bool wanted = false;
        if (npc.guard && playerEntity.is_alive()) {
            if (const auto tag = gameTags.find("Crime.Wanted")) {
                wanted = playerEntity.get<gameplay::AbilitySystem>()
                             .tags.has(*tag);
            }
        }
        bool inCombat = false;
        if ((npc.hostile || wanted) && playMode && player) {
            const Vec3 playerPos = player->position();
            Vec3 to = playerPos - transform.position;
            to.y = 0.0f;
            const f32 distance = glm::length(to);
            if (distance < 16.0f) {
                const Vec3 eye =
                    transform.position + Vec3 { 0.0f, 1.5f, 0.0f };
                const Vec3 target = playerPos + Vec3 { 0.0f, 1.2f, 0.0f };
                const Vec3 dir = glm::normalize(target - eye);
                const f32 sight = glm::length(target - eye);
                const phys::RayHit hit =
                    physics->rayCast(eye, dir, sight);
                const bool blocked = hit.hit && hit.distance < sight - 0.6f;
                if (!blocked) {
                    inCombat = true;
                    npc.sitting = false;
                    npc.attackCooldown -= dt;
                    npc.repathTimer -= dt;
                    if (distance > 1.8f) {
                        if (npc.repathTimer <= 0.0f) {
                            const nav::PathResult found =
                                navigator->findPath({ transform.position,
                                                      playerPos, 1.2f });
                            npc.path = found.success ? found.waypoints
                                                     : vector<Vec3> {};
                            npc.pathIndex = 0;
                            npc.repathTimer = 1.0f;
                        }
                        moveNpcAlongPath(npc, dt, 1.8f); // hurry
                    } else {
                        npc.path.clear();
                        // Face the player and swing.
                        const f32 goalYaw = std::atan2(to.x, to.z);
                        npc.yaw = goalYaw;
                        transform.rotation = glm::angleAxis(
                            npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
                        if (npc.attackCooldown <= 0.0f && banditWeapon &&
                            playerEntity.is_alive() && !godMode) {
                            npc.attackCooldown = 1.6f;
                            gameplay::StatBlock block {
                                playerEntity
                                    .get_mut<gameplay::CoreAttributes>(),
                                playerEntity
                                    .get_mut<gameplay::AttributeSet>(),
                                playerEntity
                                    .get_mut<gameplay::AbilitySystem>(),
                                playerEntity
                                    .get_mut<gameplay::CombatState>()
                            };
                            const gameplay::DamageResult result =
                                gameplay::applyDamage(
                                    block,
                                    gameplay::weaponDamageEvent(
                                        *banditWeapon, npcSys),
                                    gameTags, derivedStats, nullptr,
                                    statsTuning);
                            LOG_INFO("Bandit hits you: {:.0f} damage{}",
                                     result.healthDamage,
                                     result.staggered ? " (staggered!)"
                                                      : "");
                        }
                    }
                }
            }
        }

        if (inCombat) {
            // combat overrode the schedule this frame
        } else if (npc.schedule.isValid()) {
            // --- Schedule-driven day (B3) ---
            updateNpcSchedule(npc, hourOfDay);
            npc.repathTimer -= dt;
            if (npc.wanderTimer > 0.0f) {
                npc.wanderTimer -= dt;
            }
            const gameplay::AiPackageForm* package = npc.activePackage;
            Vec3 anchor = transform.position;
            if (const auto* locationRef =
                    npc.activeLocation.isValid()
                        ? forms.find<world::ReferenceForm>(
                              npc.activeLocation)
                        : nullptr) {
                anchor = locationRef->position;
                anchor.y = render::terrain::height(terrain.params, anchor.x,
                                                   anchor.z);
            }
            const auto goTo = [&](const Vec3& target) {
                if (npc.pathIndex < npc.path.size() ||
                    npc.repathTimer > 0.0f) {
                    return;
                }
                const nav::PathResult found = navigator->findPath(
                    { transform.position, target, 0.8f });
                npc.path = found.success ? found.waypoints
                                         : vector<Vec3> {};
                npc.pathIndex = 0;
                npc.repathTimer = 2.0f; // budget: no repath storm
            };
            const str kind = package ? package->kind : str { "guard" };
            if (kind == "wander") {
                const f32 radius = package ? package->radius : 4.0f;
                if (npc.pathIndex >= npc.path.size() &&
                    npc.wanderTimer <= 0.0f) {
                    // Cheap per-NPC stroll target around the anchor
                    // (cosmetic randomness — not gameplay RNG, §8).
                    const u32 hash =
                        static_cast<u32>(npc.entity.id()) * 2654435761u +
                        static_cast<u32>(timeSeconds * 0.37f);
                    const f32 angle = static_cast<f32>(hash % 628) * 0.01f;
                    const f32 reach =
                        radius * (0.35f + static_cast<f32>(hash % 61) *
                                              0.01f);
                    goTo(anchor + Vec3 { std::cos(angle) * reach, 0.0f,
                                         std::sin(angle) * reach });
                }
                if (moveNpcAlongPath(npc, dt,
                                     package ? package->speed : 1.0f)) {
                    if (npc.pathIndex >= npc.path.size() &&
                        npc.wanderTimer <= 0.0f && !npc.path.empty()) {
                        npc.path.clear();
                        npc.wanderTimer = 3.0f + static_cast<f32>(
                                                     npc.entity.id() % 4);
                    }
                    idleDecay = 6.0f;
                }
            } else if (kind == "useFurniture" || kind == "sleep" ||
                       kind == "eat" || kind == "work") {
                goTo(anchor);
                if (moveNpcAlongPath(npc, dt,
                                     package ? package->speed : 1.0f)) {
                    // Arrived: claim a point and sit (the anim graph's
                    // State.Sitting gate does the rest).
                    if (!npc.furnitureClaimed) {
                        furnitureOccupancy.claim(npc.activeLocation, 1,
                                                 npc.entity.id());
                        npc.furnitureClaimed = true;
                    }
                    npc.sitting = true;
                }
            } else { // travel / guard / unknown: reach the spot and stand
                goTo(anchor);
                moveNpcAlongPath(npc, dt, package ? package->speed : 1.0f);
            }
        } else if (patrolPoints.size() >= 2) {
            // --- Legacy patrol fallback (chantier 1 B6) ---
            const Vec3 goal =
                patrolPoints[npc.target % patrolPoints.size()];
            Vec3 to = goal - transform.position;
            to.y = 0.0f;
            const f32 distance = glm::length(to);
            if (npc.pauseTimer > 0.0f) {
                npc.pauseTimer -= dt;
            } else if (distance < 0.4f) {
                npc.pauseTimer = kNpcPauseSeconds;
                npc.target = (npc.target + 1) %
                             static_cast<u32>(patrolPoints.size());
            } else {
                npc.path = { goal };
                npc.pathIndex = 0;
                moveNpcAlongPath(npc, dt, 1.0f);
            }
        }
        npc.speed -= npc.speed * (1.0f - std::exp(-idleDecay * dt)) *
                     (npc.pathIndex >= npc.path.size() ? 1.0f : 0.0f);

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
    if (!npcs.empty()) {
        frame.cmd.setPipeline(skinnedCasterPipeline);
        frame.cmd.setBindGroup(1, casterGroup);
        for (auto& npcPtr : npcs) {
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
    w.stormFront = stormFrontUi;     // brick 30
    w.rainIntensity = rainIntensityUi; // brick 31
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
    stormFrontUi = w.stormFront;       // brick 30
    rainIntensityUi = w.rainIntensity; // brick 31
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
        sky.evaluate({ .cloudCoverage = cloudCoverageUi,
                       .sunIntensity = sunIntensityUi,
                       .ambientIntensity = ambientIntensityUi,
                       .saturation = saturationUi,
                       .warmth = warmthUi });

    // Shadows ramp out as the sun crosses the horizon (no sun, no shadows),
    // and soften away under heavy cloud cover (diffuse light casts none).
    const bool shadowsAvailable = shadows.receiverBindGroup().id != 0;
    const f32 shadowStrength =
        (shadowsUi && shadowsAvailable && !interiorMode)
            ? glm::smoothstep(-0.02f, 0.06f, skyState.sunDirection.y) *
                  (1.0f - 0.65f * cloudCoverageUi)
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
    frameData.grassBendInfo =
        playMode && player
            ? Vec4 { player->position().x, player->position().z,
                     player->position().y, 0.85f }
            : Vec4 { 0.0f };
    // Brick 30/31: the crossfaded storm front + rain intensity, and the
    // top-down rain-occlusion matrix (ortho, 40 m around the camera).
    frameData.stormInfo.x = stormFrontUi;
    frameData.stormInfo.y = interiorMode ? 0.0f : rainIntensityUi;
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
        if (!interiorMode && stormFrontUi > 0.003f) {
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
        if (playMode && promptEntity.is_alive() && fadeDirection == 0 &&
            !promptLabel.empty()) {
            const ImVec2 size = ImGui::CalcTextSize(promptLabel.c_str());
            foreground->AddText(
                { (display.x - size.x) * 0.5f, display.y * 0.62f },
                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.95f)),
                promptLabel.c_str());
        }
        if (talkTimer > 0.0f && !talkLine.empty()) {
            const ImVec2 size = ImGui::CalcTextSize(talkLine.c_str());
            foreground->AddText(
                { (display.x - size.x) * 0.5f, display.y * 0.55f },
                ImGui::GetColorU32(ImVec4(1.0f, 0.95f, 0.8f, 0.95f)),
                talkLine.c_str());
        }
    }
    if (fadeAlpha > 0.0f) {
        foreground->AddRectFilled(
            { 0.0f, 0.0f }, display,
            ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, fadeAlpha)));
    }

    // F6 toggles the level editor (leaves Play first — the editor flies).
    if (ImGui::IsKeyPressed(ImGuiKey_F6, false) && levelEditor) {
        editMode = !editMode;
        if (editMode && playMode) {
            exitPlayMode();
        }
        if (!editMode) {
            editSelection = ecs::Entity {};
            placementBase = core::Guid {};
        }
    }
    if (editMode && levelEditor) {
        drawEditorUi();
    }

    // Chantier 5 B5: quicksave / quickload.
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        performSave("quick");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        requestLoad("quick");
    }

    // F8 toggles the dev console (chantier 4 B7 — spawn/tp/tgm/settime,
    // reflection get/set, Lua).
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) {
        consoleVisible = !consoleVisible;
    }
    if (consoleVisible && console) {
        console->draw();
    }

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
        ImGui::Text("Collision tiles: %u | cells loaded: %u",
                    terrainCollision->tileCount(),
                    cellStreamer ? cellStreamer->loadedCount() : 0);
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
