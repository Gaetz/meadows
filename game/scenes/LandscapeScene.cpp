#include "game/scenes/LandscapeScene.hpp"

#include <algorithm>
#include <chrono> // C9.7: save/load timing baselines
#include <cmath>
#include <ctime>
#include <filesystem>
#include <sstream>

#include <glm/glm.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "game/AllForms.hpp"
#include "game/Barter.hpp"
#include "game/SceneStack.hpp"        // Edit mode pushes overlays (host())
#include "game/scenes/EditorScene.hpp" // the Game DB overlay
#include "game/ui/ConsolePanel.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/Engine.hpp"
#include "engine/FrameContext.hpp"
#include "engine/core/Log.hpp"
#include "engine/platform/Input.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/platform/Window.hpp"
#include "data/forms/AnimForms.hpp"
#include "data/forms/CoreForms.hpp"
#include "data/forms/UiForms.hpp"
#include "engine/rhi/Device.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/combat/MeleeSwing.hpp" // P0 A3: the viewmodel swing arc
#include "gameplay/cue/GameplayCues.hpp"  // P0 C1: toEmitterParams (fx cmd)
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/actors/CharacterTick.hpp"
#include "gameplay/actors/FollowerForms.hpp" // É5: spawn-time class curves
#include "gameplay/actors/Followers.hpp"     // É5: applyFollowerClass
#include "gameplay/interaction/FurnitureForms.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/Rest.hpp"
#include "game/WeaponMeshes.hpp" // A2: the procedural sword
#include "script/Vm.hpp"
#include "world/scene/AnimBridge.hpp"
#include "world/scene/KillZ.hpp"
#include "world/scene/Spawner.hpp"
#include "world/scene/TriggerSystem.hpp"
#include "world/terrain/TerrainPatches.hpp"

namespace game {

// (B5.5 stat->world movement constants moved to PlayerController, audit
// U4-1; the tonemap constant and the oblique-projection helper moved to
// LandscapeRenderer, audit U4-2c.)

void LandscapeScene::onEnter() {
    // C9.7: the load-side measurement baseline — a load IS a scene
    // reload (F9 = onExit + onEnter), so its total is timed here;
    // bootstrapData logs the parse and resolve slices.
    const auto enterStart = std::chrono::steady_clock::now();
    rhi::Device& device = engine->getDevice();
    bootstrapData();
    createRenderResources(device);
    setupGameplay();
    setupWorldAndStreaming();
    spawnInitialWorld(device);
    LOG_INFO("Load: scene rebuild {:.1f} ms total",
             std::chrono::duration<f64, std::milli> {
                 std::chrono::steady_clock::now() - enterStart }
                 .count());
}

// C9.5: the plugin config with the language gate applied — every
// text-<code>.toml pack (English is the BASE, never a pack) is enabled
// exactly when <code> is settings.language. Shared by bootstrapData and
// the options screen's live language switch.
data::PluginConfig LandscapeScene::loadGatedPluginConfig(
    const std::filesystem::path& dataDir) const {
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
    for (auto& entry : pluginConfig.entries) {
        if (const auto code = languagePackCode(entry.file)) {
            entry.enabled = (*code == settings.language);
        }
    }
    return pluginConfig;
}

void LandscapeScene::bootstrapData() {
    // C9.2: machine preferences FIRST (settings.toml beside saves/) —
    // bindings land in the ActionMap, the deadzone feeds the input layer,
    // and (C9.5) settings.language gates the language-pack plugins below.
    // Reload on every enter is harmless: the file is the source of truth.
    loadSettings(settingsPath(), settings, actionMap);
    engine->getInput().setStickDeadzone(settings.stickDeadzone);
    // Load the moddable data (§5) through the plugin stack (chantier 4 B1):
    // data/plugins.toml declares the load order, the resolver layers every
    // plugin's fields last-writer-wins. One registration site for all
    // families (AllForms) — UI/quest/dialogue records now resolve here too.
    game::registerAllFormTypes(formTypes);
    const auto dataDir = platform::executableDir() / "data";
    const data::PluginConfig pluginConfig = loadGatedPluginConfig(dataDir);
    // C9.7: load-side timings (the chantier's measurement baseline) —
    // the TOML parse of the whole stack, then the §5 resolve.
    const auto parseStart = std::chrono::steady_clock::now();
    pluginStack = data::loadPluginStack(dataDir, pluginConfig, formTypes);
    LOG_INFO("Load: plugin stack parsed in {:.1f} ms ({} plugins)",
             std::chrono::duration<f64, std::milli> {
                 std::chrono::steady_clock::now() - parseStart }
                 .count(),
             pluginStack.plugins.size());
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
    const auto resolveStart = std::chrono::steady_clock::now();
    data::resolve(loadOrder, formTypes, forms);
    LOG_INFO("Load: resolve {:.1f} ms ({} forms)",
             std::chrono::duration<f64, std::milli> {
                 std::chrono::steady_clock::now() - resolveStart }
                 .count(),
             forms.count());
    for (const data::Plugin& plugin : pluginStack.plugins) {
        for (const data::AssetEntry& entry : plugin.assets) {
            assetDb.add(entry.id, plugin.baseDir, entry.path);
        }
    }
    LOG_INFO("Plugin stack: {} plugins, {} forms",
             pluginStack.plugins.size(), forms.count());
    tuning = resolveLandscapeTuning(forms);
    weather.init(forms);
    texts.build(forms); // U4-11: LocStringForm index (key -> text)
    LOG_INFO("Loc: {} strings, language '{}' (packs gated in plugins.toml)",
             texts.size(), settings.language);
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

    // Terrain shape + startup values for every live-adjustable knob: the
    // renderer's half (terrain/exposure/ssao/grade) through applyTuning,
    // the atmosphere half here (the weather crossfade owns `atmos`).
    renderer.applyTuning(tuning, heightPatches);
    atmos.fogDensity = tuning.fogDensity;
    atmos.fogHeightFalloff = tuning.fogHeightFalloff;
    atmos.fogLowBoost = tuning.fogLowBoost;
    atmos.fogStart = tuning.fogStart;
    atmos.bloomIntensity = tuning.bloomIntensity;
    atmos.godRayIntensity = tuning.godRayIntensity;
    atmos.volumetric = tuning.volumetricIntensity;
    atmos.cloudCoverage = tuning.cloudCoverage;
    atmos.cloudShadow = tuning.cloudShadowStrength;
    atmos.cloudHeight = tuning.cloudHeight;
    atmos.cloudScale = tuning.cloudScale;
}

void LandscapeScene::createRenderResources(rhi::Device& device) {
    // B1: the real mesh path. Plugin ReferenceForms spawn into a small ECS
    // world through the Spawner (§2.7 — MeshRender wired by reflection from
    // the base form's model/material); extractMeshes fills the snapshot;
    // the residency caches resolve guids at draw time (§7). The caches are
    // SCENE-owned (editor and streaming share them); everything else GPU
    // belongs to the renderer (U4-2c).
    materialTextures = std::make_unique<TextureCache>(
        device, assetDb, engine->getJobSystem(),
        TextureCache::UploadDesc { .format = rhi::TextureFormat::SRGBA8,
                                   .filter = rhi::FilterMode::Linear });
    meshCache =
        std::make_unique<MeshCache>(device, assetDb, engine->getJobSystem());
    // Chantier P0 A2: the procedural weapons — the sword is the default
    // (WeaponForms without a `model` fall back to it); the club is what
    // BanditClub's `model` points at in data.
    meshCache->injectProcedural(swordMeshGuid(), makeSwordMesh(0.9f));
    meshCache->injectProcedural(clubMeshGuid(), makeClubMesh(0.8f));
    meshCache->injectProcedural(bowMeshGuid(), makeBowMesh(1.3f));   // A7
    meshCache->injectProcedural(arrowMeshGuid(), makeArrowMesh(0.6f));
    renderer.create(device, engine->getJobSystem());

    // Chantier 4 B2: the RmlUi game UI (screens from UiScreenForm records,
    // documents through the plugins' ui/ roots).
    createGameUi(device);
}

void LandscapeScene::setupGameplay() {
    // B4: the sim-side physics world + terrain collision (tiles follow the
    // camera for now; the player becomes the focus in B5).
    physics = std::make_unique<phys::PhysicsWorld>();
    terrainCollision = std::make_unique<TerrainCollision>(
        *physics, renderer.terrainParams(), &engine->getJobSystem());
    vegCollision =
        std::make_unique<VegetationCollision>(*physics, renderer.terrainParams());

    // Chantier 3 B2: navigation over the SAME height function as
    // everything else (patches included — the pointer rides in params).
    navigator = std::make_unique<world::TerrainNavigator>(
        [this](f32 x, f32 z) {
            return render::terrain::height(renderer.terrainParams(), x, z);
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
    swimCostEffect = // D2b: the swim drain (combat.toml)
        data::findByEditorId<gameplay::EffectForm>(forms, "SwimCost");
    sneakCostEffect = // sneak: the moving drain (combat.toml)
        data::findByEditorId<gameplay::EffectForm>(forms, "SneakCost");
    bowDrawCostEffect = // A7+: the drawn-bow drain (combat.toml)
        data::findByEditorId<gameplay::EffectForm>(forms, "BowDrawCost");
    testWoundEffect =
        data::findByEditorId<gameplay::EffectForm>(forms, "TestLegWound");
    // Chantier 3 B6: the melee weapons (data — retune in village.toml).
    playerWeapon =
        data::findByEditorId<data::WeaponForm>(forms, "RustySword");
    banditWeapon =
        data::findByEditorId<data::WeaponForm>(forms, "BanditClub");
    // P0 A3: the melee attack ability — SHARED by player and NPCs; its
    // cost/cooldown effects (§6) are the only cadence gates.
    attackAbility =
        data::findByEditorId<gameplay::AbilityForm>(forms, "PlayerAttack");
    // Dodge (dev design 2026-07-11): the 2D arena ability, now in 3D —
    // cost/cooldown/i-frames all live in its effects.
    dodgeAbility =
        data::findByEditorId<gameplay::AbilityForm>(forms, "Dodge");
    // P0 C3: the audio backend + the SoundForm resolver (idempotent on
    // re-enter: create() is a no-op once ready).
    if (!audioSystem.ready()) {
        audioSystem.create();
    }
    // C9.4: the persisted master volume applies at boot (settings were
    // loaded in bootstrapData); the options screen re-applies on change.
    applyMasterVolume(audioSystem, settings.masterVolume);
    soundResolver.create(forms, assetDb, &audioSystem);
    // P0 C2: cue handlers over the resolved CueForms — combat feedback
    // (hit sparks, parry shake, C3 sounds) is data from here on.
    fxDirector.create(forms, fxSim, &soundResolver);
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
    // 8.7c: ONE generic subscription — quest starts (QuestForm.startEvent)
    // and task progression are open, data-defined vocabularies; the
    // director filters, not the wiring. Modded quests need zero C++.
    eventBus.subscribeAll([this](const gameplay::Event& event) {
        questDirector.handleQuestEvent(makeQuestContext(), event);
    });
    eventBus.subscribe(gameplay::eventKind("OnPayFine"),
                       [this](const gameplay::Event&) {
                           questDirector.payFine(makeQuestContext());
                       });
    // FOLLOWERS É1: recruit/dismiss ride the same dialogue-event channel
    // as OpenBarter/OnPayFine — the partner is whoever [E] Talk opened.
    eventBus.subscribe(gameplay::eventKind("OnRecruitFollower"),
                       [this](const gameplay::Event&) {
                           followerController.recruit(
                               makeFollowerContext(),
                               questDirector.dialoguePartner());
                       });
    eventBus.subscribe(gameplay::eventKind("OnDismissFollower"),
                       [this](const gameplay::Event&) {
                           followerController.dismiss(
                               makeFollowerContext(),
                               questDirector.dialoguePartner());
                           // A dismiss to a non-resident home despawns the
                           // entity: prune the director list NOW (update()
                           // assumes live entities).
                           refreshNpcs(engine->getDevice());
                       });
    // FOLLOWERS É2: the aggro table rides the signals combat ALREADY
    // publishes (§2.11 — resolveMeleeStrike/resolveStrikeDamage dispatch
    // OnHitTaken, the director dispatches OnDeath): followers defend the
    // player, hostiles fight back, a death disengages whoever targeted it.
    eventBus.subscribe(gameplay::eventKind("OnHitTaken"),
                       [this](const gameplay::Event& event) {
                           followerController.onHitTaken(
                               makeFollowerContext(), event);
                       });
    eventBus.subscribe(gameplay::eventKind("OnDeath"),
                       [this](const gameplay::Event& event) {
                           followerController.onDeath(makeFollowerContext(),
                                                      event);
                       });
    // FOLLOWERS É3: the consultation dialogue option ("Comment te
    // sens-tu ?") rides the same dialogue-event channel as
    // recruit/dismiss; v1 answer = a HUD toast (health %, injuries,
    // remaining rest), no new screen.
    eventBus.subscribe(gameplay::eventKind("OnFollowerStatus"),
                       [this](const gameplay::Event&) {
                           followerController.consultFollower(
                               makeFollowerContext(),
                               questDirector.dialoguePartner());
                       });
    // FOLLOWERS É4: « Parle-moi de tes aptitudes » — the recruit-preview
    // screen on the same dialogue-event channel (partner = whoever [E]
    // Talk opened).
    eventBus.subscribe(gameplay::eventKind("OnFollowerPreview"),
                       [this](const gameplay::Event&) {
                           followerController.openRecruitPreview(
                               makeFollowerContext(),
                               questDirector.dialoguePartner());
                       });
    // FOLLOWERS É4: affinity rules — ONE generic subscription (the 8.7c
    // subscribeAll precedent): AffinityRuleForm children of each follower's
    // ActorForm name the events they react to (open, data-defined
    // vocabulary — modded rules need zero C++).
    eventBus.subscribeAll([this](const gameplay::Event& event) {
        followerController.onAffinityEvent(makeFollowerContext(), event,
                                           questDirector.dialoguePartner());
    });
    // P0 B2 hearing: any OnNoise event turns nearby perceivers'
    // heads — the noise position is the SOURCE entity's transform.
    eventBus.subscribe(
        gameplay::eventKind("OnNoise"), [this](const gameplay::Event& event) {
            if (event.source.is_alive() &&
                event.source.has<world::Transform>()) {
                npcDirector.onNoise(
                    event.source.get<world::Transform>().position);
            }
        });
    // P0 C4b: footsteps become MATERIAL cues + noise. "Footstep"
    // AnimEvents flow from the NPC clips (C4a) and the player's stride
    // synthesizer; the material is the terrain splat's verdict (ONE
    // definition of what grows where) — interiors step on wood. The
    // CueTable's hierarchical fallback covers unauthored materials
    // (Cue.Footstep.Snow -> Cue.Footstep).
    eventBus.subscribe(
        gameplay::eventKind("AnimEvent"),
        [this](const gameplay::Event& event) {
            if (event.name != "Footstep" || !event.source.is_alive() ||
                !event.source.has<world::Transform>()) {
                return;
            }
            const Vec3 at = event.source.get<world::Transform>().position;
            const char* material = "Wood";
            if (!interiorMode) {
                // The SHADED weights (wander included): the step sounds
                // like the ground LOOKS, not like the altitude contour.
                // The scene queries the terrain (it owns the renderer);
                // the weights -> name verdict lives on FxDirector (R6).
                const auto weights =
                    render::terrain::materialWeightsShaded(
                        renderer.terrainParams(), at.x, at.z,
                        tuning.splatUvScale);
                material = FxDirector::footstepMaterial(weights);
            }
            // Sneaked steps are softer, lower and carry half as far.
            const bool sneaked = event.source == playerEntity &&
                                 playerController.sneaking();
            gameplay::CueEvent step { str { "Cue.Footstep." } + material,
                                      at, 0.0f };
            if (sneaked) {
                step.volumeScale = statsTuning.sneakVolumeFactor;
                step.pitchScale = statsTuning.sneakPitchFactor;
            }
            fxDirector.cues().emit(step);
            // Only the PLAYER'S steps are heard (B2 sneaking hook):
            // villagers must not investigate each other's strolls.
            if (event.source == playerEntity) {
                npcDirector.onNoise(
                    at, sneaked ? statsTuning.sneakDetectionFactor : 1.0f);
            }
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
        // FOLLOWERS É1: a re-homed reference (recruited -> cell 0) must
        // not respawn from its authored cell — the live entity travels
        // with the player (PendingSaveLayer::isRehomed).
        return saveController.pending().isEnabled(referenceId) &&
               !saveController.pending().isRehomed(referenceId);
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
            // A7: the bow rides in the bag too (equip it to fire),
            // with a starting quiver of arrows.
            if (const auto* bow = data::findByEditorId<data::WeaponForm>(
                    forms, "HuntingBow");
                bow && gameplay::itemCount(bag, bow->id) == 0) {
                gameplay::addItem(bag, bow->id, 1);
                if (bow->ammo.isValid()) {
                    gameplay::addItem(bag, bow->ammo, 12);
                }
            }
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
    // and its patrol brain. The scene builds no character by hand anymore
    // (the skinned shader/pipeline live in the renderer, U4-2c).

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

    // (Brick 23 rock override, sky/shadows/water/reflection/blit/postFx/
    // gpuOcclusion creation: all moved into LandscapeRenderer::create.)

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
        const f32 ground = render::terrain::height(renderer.terrainParams(), 32.0f,
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
        const Vec3 eye { 0.0f, statsTuning.eyeHeight, 0.0f }; // U4-7
        Vec3 feet = flyCamera.camera.position - eye;
        if (playerEntity.is_alive()) {
            feet = playerEntity.get<world::Transform>().position;
        }
        flyCamera.camera.position = feet + eye;
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
    optionsController.reset(); // C9.4: drop any armed rebind capture
    mapController.reset(); // C9.6: runtime pixels died with the UiSystem
    followerController.reset(); // É4: drop the affinity-accrual stamp
    goldForm = nullptr;
    sceneConsole.reset(); // panel/VM/session reference forms — before re-resolve
    // U4-2c: every GPU resource and render system is the renderer's; the
    // scene keeps the caches (their dtors free what they own — device is
    // alive here) and the sim teardown.
    renderer.destroy(device);
    snapshot = RenderSnapshot {};
    meshCache.reset();
    materialTextures.reset();
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
    // C9.7: async disk saves complete at the same fixed point — the
    // timing log and the save.saved toast fire here, at completion. A
    // save requested while one was in flight (F5 spam, last slot wins)
    // relaunches NOW with a FRESH capture of the current world.
    if (const auto slot = saveController.pumpCompletions(
            texts, [this](const str& m) { interaction.say(m, 3.0f); })) {
        saveController.performSave(makeSaveContext(), *slot);
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
    renderer.skySystem().timeOfDay =
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
        // Chantier « volumes de gameplay » : loaded TriggerVolumes vs
        // actor positions — enter/leave fire the volume's event on the
        // scene bus (quests/dialogue subscribe there) and its Lua snippet
        // through the shared VM. NPC positions are last frame's (they
        // move in updateNpcs, later) — one frame of latency is fine.
        {
            // P0 B1: ONE actor snapshot per frame — the trigger sweep
            // and the faction shout (callForHelp, R3) query it. Hearing
            // stays a per-perceiver sweep (its radius is per-NPC).
            spatialIndex.rebuild(world);
            world::TriggerCallbacks triggerCb;
            triggerCb.events = &eventBus;
            triggerCb.runScript = [this](const str& code, ecs::Entity actor,
                                         ecs::Entity) {
                script::Vm* vm = sceneConsole.vm();
                if (!vm) {
                    return;
                }
                script::ScriptContext ctx;
                ctx.entity = actor;
                if (actor.has<gameplay::AttributeSet>()) {
                    ctx.attributes =
                        &actor.get_mut<gameplay::AttributeSet>();
                }
                if (actor.has<gameplay::AbilitySystem>()) {
                    ctx.abilitySystem =
                        &actor.get_mut<gameplay::AbilitySystem>();
                }
                ctx.tags = &gameTags;
                ctx.forms = &forms;
                if (const auto result = vm->run(code, ctx); !result.ok) {
                    LOG_WARN("Trigger script failed: {}", result.error);
                }
            };
            world::updateTriggerVolumes(world, triggerCb, &spatialIndex);
        }
    }
    {
        // U4-2a: everything render() needs from the World is extracted HERE —
        // the render path reads only the snapshot (the Phase-5 seam).
        core::FrameProbe::Scope probe { frameProbe, "extract" };
        snapshot.meshes.clear();
        snapshot.lights.clear();
        snapshot.shadowLights.clear();
        snapshot.shafts.clear();
        snapshot.waterVolumes.clear();
        extractMeshes(world, snapshot);
        resolveMeshMaterials(forms, snapshot);
        extractLights(world, flyCamera.camera.position,
                      LandscapeRenderer::kMaxLights, snapshot);
        extractWaterVolumes(world, snapshot);
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
        // FOLLOWERS É3/É4: after the tick mirrored State.Downed, run the
        // follower sweep — protection sync, time-together affinity (É4),
        // bleedout clock, resolution (recover / convalescence dismiss /
        // real death). A dismiss to a non-resident home despawns the
        // entity: prune the list NOW.
        if (followerController.updateFollowers(makeFollowerContext(), dt)) {
            refreshNpcs(engine->getDevice());
        }
        // P0 A7: arrows fly with the sim.
        projectileDirector.update(
            dt, ProjectileContext { physics.get(), npcDirector.npcs(),
                                    playerEntity, playerController.body(),
                                    gameTags, derivedStats, statsTuning,
                                    eventBus, &fxDirector.cues(),
                                    sceneConsole.godMode() });
        // P0 C1: the particle sim advances with the world (paused sim =
        // frozen sparks, like everything else).
        fxSim.update(dt);
        // P0 D2a: the kill-z floor (WorldspaceForm.killZ) — falling out
        // of the world is an outright death through the NORMAL pipeline
        // (dev: mort franche), never a teleport-back. The sweep itself
        // is headless in world/scene/KillZ (audit R6); the scene only
        // resolves the worldspace's killZ (fallback = the Form's own
        // default, never a re-hardcoded literal) and gates the player.
        f32 killZ = world::WorldspaceForm {}.killZ;
        if (const data::Form* space = forms.get(activeWorldspace)) {
            const reflect::TypeInfo* type = forms.typeOf(activeWorldspace);
            if (type &&
                type->isA(world::WorldspaceForm::staticTypeInfo().id)) {
                killZ = static_cast<const world::WorldspaceForm*>(space)
                            ->killZ;
            }
        }
        const bool sweepPlayer = mode == SceneMode::Play &&
                                 playerController.body() &&
                                 !sceneConsole.godMode();
        world::enforceKillZ(world, killZ, gameTags, derivedStats,
                            statsTuning,
                            sweepPlayer ? ecs::Entity {} : playerEntity);
    }
    // P0 C2: shake decay + the transient camera offset (removed first
    // each frame, so paused sims and fly cameras never accumulate it).
    fxDirector.update(dt, flyCamera);
    // P0 C3: reap finished one-shots; the listener rides the camera.
    audioSystem.update(dt);
    audioSystem.setListener(flyCamera.camera.position,
                            flyCamera.camera.forward());
    // P0 C1: live particles -> POD batches; the ALPHA batch is sorted
    // far-to-near around the camera (additive needs no order).
    snapshot.fxAlpha.clear();
    snapshot.fxAdditive.clear();
    fxSim.forEach([&](const Vec3& position, f32 size, const Vec4& color,
                      bool additive) {
        (additive ? snapshot.fxAdditive : snapshot.fxAlpha)
            .push_back({ Vec4 { position, size }, color });
    });
    {
        const Vec3 eye = flyCamera.camera.position;
        std::sort(snapshot.fxAlpha.begin(), snapshot.fxAlpha.end(),
                  [&](const render::FxInstance& a,
                      const render::FxInstance& b) {
                      const Vec3 da = Vec3 { a.positionSize } - eye;
                      const Vec3 db = Vec3 { b.positionSize } - eye;
                      return glm::dot(da, da) > glm::dot(db, db);
                  });
    }
    // U4-2b: the skinned extract runs AFTER the NPC update so the packet
    // carries this frame's pose (paused sim: the last pose, still valid).
    snapshot.skinned.clear();
    npcDirector.extract(snapshot);
    projectileDirector.extract(snapshot); // A7: arrows in flight/planted
    // Chantier P0 A2/A3: the first-person viewmodel — the player's sword
    // rides the simulated swing socket (guard pose bottom-right when
    // Idle; the LMB swing sweeps it right-to-left along the same arc the
    // hit test travels). Socket constants live in swingSocketLocal
    // [cpp-tuning: dev visual pass].
    if (mode == SceneMode::Play && playerWeapon &&
        playerEntity.is_alive() && playerController.weaponDrawn()) {
        const render::Camera3D& cam = flyCamera.camera;
        const Vec3 fwd = cam.forward();
        const Vec3 right = cam.right();
        const Vec3 up = glm::normalize(glm::cross(right, fwd));
        const Mat4 basis { Vec4 { right, 0.0f }, Vec4 { up, 0.0f },
                           Vec4 { -fwd, 0.0f },
                           Vec4 { cam.position, 1.0f } };
        // The viewmodel shows the EQUIPPED weapon (loot a club, equip
        // it, see it) — the swing's weapon while one is in flight, the
        // scene fallback only without any equipment.
        const data::WeaponForm* shown = playerWeapon;
        if (playerController.swingWeapon()) {
            shown = playerController.swingWeapon();
        } else if (playerEntity.has<gameplay::Equipment>()) {
            const auto& equipment = playerEntity.get<gameplay::Equipment>();
            if (equipment.weapon.isValid()) {
                if (const auto* form =
                        forms.find<data::WeaponForm>(equipment.weapon)) {
                    shown = form;
                }
            }
        }
        const data::WeaponForm& weapon = *shown;
        const gameplay::SwingTiming timing { weapon.swingWindup,
                                             weapon.swingActive,
                                             weapon.swingRecovery };
        const auto& swing = playerEntity.get<gameplay::MeleeSwing>();
        // A raised guard shows the parry pose (sword oblique across the
        // front); otherwise the socket follows the swing arc.
        const Mat4 pose =
            basis * (swing.guardSeconds >= 0.0f
                         ? gameplay::guardSocketLocal()
                         : gameplay::swingSocketLocal(swing, timing));
        const core::Guid model =
            weapon.model.isValid() ? weapon.model : swordMeshGuid();
        snapshot.meshes.push_back({ model, core::Guid {}, pose });
        // A7+: while the bow is drawn, a nocked arrow rides the viewmodel
        // and slides BACK with the charge ("la flèche recule") — Rx(-90°)
        // turns the +Y shaft toward -Z (the aim direction).
        const f32 charge = playerController.bowCharge();
        if (charge >= 0.0f) {
            const Mat4 arrowLocal =
                glm::translate(Mat4 { 1.0f },
                               Vec3 { 0.13f, -0.30f,
                                      -0.42f + charge * 0.22f }) *
                glm::rotate(Mat4 { 1.0f }, glm::radians(-90.0f),
                            Vec3 { 1.0f, 0.0f, 0.0f });
            snapshot.meshes.push_back(
                { arrowMeshGuid(), core::Guid {}, basis * arrowLocal });
        }
    }
    // Wind phase integrates the CURRENT strength: speed changes bend the
    // drift/sway smoothly instead of teleporting the pattern.
    windTime += dt * glm::max(atmos.windStrength, 0.05f);

    // Weather crossfade (owned by WeatherController): slides `atmos` from the
    // captured start state to the selected weather over its duration.
    weather.update(atmos, dt);

    // Mode switching lives with the F2/F3 hotkeys (drawn overlay); Play is
    // home. Nothing to toggle here anymore.
    if (uiPaused || uiPadConsumedA || uiPadConsumedB) {
        // A modal screen owns the input; cameras and player hold still.
        // C9.3: a pad edge the UI consumed keeps its button until the
        // physical release — otherwise closing a menu with B would
        // tap-dodge, and activating with A would jump, the same frame.
    } else if (sceneConsole.visible()) {
        // The dev console owns the keyboard; the player / camera hold still
        // (so WASD types instead of walking) while the sim keeps ticking.
    } else if ((mode == SceneMode::Play) && playerController.body()) {
        playerController.update(dt, makePlayerContext());
    } else {
        // Don't steal the mouse from ImGui: clicking a panel must not
        // mouselook. Spectator AND Edit look only while RMB is held (dev
        // decision 2026-07-10: the cursor stays free by default in both,
        // for the panels/buttons); only the no-capsule Play fallback
        // keeps continuous mouselook. Hold Alt to free the cursor
        // mid-look without releasing the button.
        const bool freeMouse = ImGui::GetIO().KeyAlt;
        const bool allowCapture =
            !ImGui::GetIO().WantCaptureMouse && !freeMouse;
        const auto trigger =
            (mode == SceneMode::Play)
                ? render::FlyCamera::LookTrigger::Always
                : render::FlyCamera::LookTrigger::RightButton;
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

void LandscapeScene::enterPlayMode() {
    if (!physics) {
        return;
    }
    // Spawn the capsule under the camera, feet grounded on the height
    // function (+0.5 m so a slope never pins the spawn into the field).
    Vec3 feet = flyCamera.camera.position;
    feet.y = render::terrain::height(renderer.terrainParams(), feet.x, feet.z) + 0.5f;
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

// THE mode transition (dev rule, 2026-07-10: one mode REPLACES the
// other — every switch funnels here, hotkeys and menu resume alike, and
// the per-mode side effects live in the two flat switches below, not in
// nested ifs at the call sites).
void LandscapeScene::restoreMode(SceneMode target) {
    if (mode == target) {
        return;
    }
    // Leave the current mode.
    switch (mode) {
    case SceneMode::Play:
        exitPlayMode(); // tears down the capsule, frees the mouse, hides HUD
        break;
    case SceneMode::Edit:
        sceneEditor.deselect();
        break;
    case SceneMode::Spectator:
        break;
    }
    // Enter the target.
    switch (target) {
    case SceneMode::Play:
        if (sceneConsole.visible()) {
            // Play starts CLEAN: an open console would keep the keyboard
            // focus and eat WASD while the mouse is captured (the
            // "cannot get back into the game" dev report) — ` reopens it.
            sceneConsole.toggle(true);
        }
        enterPlayMode(); // spawns the capsule, grabs the mouse, shows HUD
        break;
    case SceneMode::Spectator:
        mode = SceneMode::Spectator;
        // Spectator flies like Edit: free cursor (LMB-look), no console.
        engine->getWindow().setRelativeMouseMode(false);
        if (sceneConsole.visible()) {
            sceneConsole.toggle(false);
        }
        break;
    case SceneMode::Edit:
        mode = SceneMode::Edit;
        if (!sceneConsole.visible()) {
            sceneConsole.toggle(false); // Edit opens the console by default
        }
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
        renderer.terrainParams(),
        heightPatches.get(),
        forms,
        *levelEditor,
        [this](std::shared_ptr<render::HeightPatches> next,
               const std::vector<u64>& changed, bool commit) {
            // Show the new heights immediately: swap the live overlay and queue
            // a terrain re-mesh of just the changed chunks (deferred to the
            // safe point in render(), seamless swap). This runs every preview
            // frame during a stroke.
            renderer.terrainParams().patches = next;
            renderer.sculptRemeshQueue().insert(renderer.sculptRemeshQueue().end(), changed.begin(),
                                     changed.end());
            if (!commit) {
                return; // live preview: terrain only, no heavy churn
            }
            // Stroke release — the permanent publish: the overlay becomes the
            // committed one, grass/veg re-scatter onto the new heights, and
            // collision / cell snap rebuild.
            heightPatches = next;
            renderer.sculptScatterQueue().insert(renderer.sculptScatterQueue().end(),
                                       changed.begin(), changed.end());
            renderer.invalidateOcclusion();
            terrainCollision = std::make_unique<TerrainCollision>(
                *physics, renderer.terrainParams(), &engine->getJobSystem());
            vegCollision = std::make_unique<VegetationCollision>(
                *physics, renderer.terrainParams());
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
        renderer.terrainParams(),  forms,           *levelEditor,    activeWorldspace,
        worldModel,      categories,      *cellLoader,     *cellStreamer,
        spawner,         makeSculptContext(),
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
        texts,
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
        // É3: [E] on a downed follower — potion from the bag, his own
        // heal effect through applyEffect (§2.9), back on his feet.
        [this](ecs::Entity ally) {
            followerController.reviveDownedAlly(makeFollowerContext(),
                                                ally);
        },
        &actionMap, // C9.2: [E]/[X] through the action layer
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
        *physics, renderer.terrainParams(), &engine->getJobSystem());
    vegCollision =
        std::make_unique<VegetationCollision>(*physics, renderer.terrainParams());

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
    // FOLLOWERS É1: the party arrives with you — any active follower left
    // beyond the teleport radius pops in next to the arrival marker.
    followerController.repositionActiveFollowers(makeFollowerContext(),
                                                 marker->position);
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
    uiCreated = uiSystem.create(device, renderer.shaderLibrary(), roots,
                                static_cast<u32>(engine->getWindow().width()),
                                static_cast<u32>(engine->getWindow().height()));
    if (!uiCreated) {
        LOG_WARN("Game UI unavailable (UiSystem creation failed)");
        return;
    }
    // C9.5: the loc pass — BEFORE any document loads, so the preload below
    // localizes on the way in. The lambda closes over the scene's
    // TextTable (rebuilt on resolve/language switch); meadows-ui itself
    // never sees data/.
    uiSystem.setLocalizer(
        [this](std::string_view key) { return texts.get(key); });
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
          // Vitals bars (dev design 2026-07-12): per stat, the OUTER bar
          // width scales with the THEORETICAL max (1000 = the full
          // half-screen container), plus fill / resonance-bonus /
          // resonance-malus segments as % of that bar.
          .numbers = { "chargePct", // A7+: the bow-draw gauge
                       "healthBarPct",  "healthFillPct",  "healthBonusPct",
                       "healthMalusLeft",  "healthMalusPct",
                       "energyBarPct",  "energyFillPct",  "energyBonusPct",
                       "energyMalusLeft",  "energyMalusPct",
                       "essenceBarPct", "essenceFillPct", "essenceBonusPct",
                       "essenceMalusLeft", "essenceMalusPct",
                       "postureBarPct", "postureFillPct", "postureBonusPct",
                       "postureMalusLeft", "postureMalusPct" },
          .strings = { "healthText", "energyText", "essenceText",
                       "postureText", "clock", "prompt", "talk" },
          .bools = { "promptVisible", "talkVisible", "chargeVisible" },
          .rows = true }); // B7: nameplates over hostile/hurt NPCs
    // FOLLOWERS É2: the party frame — one row per ACTIVE follower (name +
    // health), its own model (a document allows one rows array per model).
    uiSystem.createModel({ .name = "party", .rows = true });
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
    // C9.4: the options screen — look/audio steppers + the bindings
    // table (one row per action; clicking a row arms a rebind capture).
    // C9.5 adds the language toggle.
    uiSystem.createModel({ .name = "options",
                           .strings = { "mouseSensText", "stickSensText",
                                        "deadzoneText", "volumeText",
                                        "invertText", "languageText" },
                           .bools = { "capturing" },
                           .rows = true,
                           .events = { "adjust", "toggleInvert",
                                       "toggleLanguage", "rebind",
                                       "optionsBack" } });
    // C9.6: the map — marker/POI positions as percentages of the raster
    // (data-style-left/top), door POIs as rows.
    uiSystem.createModel({ .name = "map",
                           .numbers = { "playerX", "playerY" },
                           .bools = { "playerVisible", "interiorNote" },
                           .rows = true,
                           .events = { "mapBack" } });
    // FOLLOWERS É4: the recruit-preview panel — name/class/level/affinity
    // + vitals as preformatted loc strings, the 9 attributes as rows.
    uiSystem.createModel({ .name = "recruit",
                           .strings = { "name", "classText", "levelText",
                                        "affinityText", "healthText",
                                        "energyText", "essenceText" },
                           .rows = true,
                           .events = { "recruitBack" } });
    // C9.6: seed runtime://map with a placeholder BEFORE the preload
    // below — a backend load that finds no pixels fails, and RmlUi
    // latches failed sources forever (FileTextureDatabase never
    // retries). One dark texel is enough; MapController pushes the real
    // raster before the screen ever shows.
    const u8 mapPlaceholder[4] = { 20, 22, 26, 255 };
    uiSystem.setRuntimeTexture("map", mapPlaceholder, 1, 1);
    uiSystem.setModelEventHandler(
        [this](const str& model, const str& event, const vector<str>& args) {
            if (model == "options") { // C9.4: settings territory
                optionsController.handleEvent(makeOptionsContext(), event,
                                              args);
                return;
            }
            if (model == "map") { // C9.6
                mapController.handleEvent(makeMapContext(), event);
                return;
            }
            if (model == "recruit") { // FOLLOWERS É4: close is all it does
                if (event == "recruitBack") {
                    screenStack.closeTop();
                }
                return;
            }
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

    // C9.3: A/B edges the UI consumed release their gameplay hold the
    // frame the button physically comes back up (see update()'s gate).
    if (uiPadConsumedA && !input.padDown(platform::PadButton::A)) {
        uiPadConsumedA = false;
    }
    if (uiPadConsumedB && !input.padDown(platform::PadButton::B)) {
        uiPadConsumedB = false;
    }
    // C9.4: an armed rebind capture owns the WHOLE input frame — it eats
    // the first pressed key/button here, BEFORE the Tab close, the Pause
    // toggle and the C9.3 pad->UI routing below, so binding B/Start/Tab
    // to an action doesn't also close or navigate the screen, and the
    // Escape cancel doesn't also toggle pause. Sampled at frame start so
    // the frame that completes a capture stays fully consumed.
    const bool capturingBind = optionsController.capturing();
    if (capturingBind) {
        // A/B grabbed by a capture stay UI-owned until physically
        // released (the C9.3 idiom) — the captured tap must not dodge
        // or jump when the screen closes.
        if (input.padPressed(platform::PadButton::A)) {
            uiPadConsumedA = true;
        }
        if (input.padPressed(platform::PadButton::B)) {
            uiPadConsumedB = true;
        }
        optionsController.updateCapture(makeOptionsContext());
    }
    // C9.3: while a modal is open the d-pad drives the RmlUi focus, so
    // a pad-bound hotkey must not ALSO fire its toggle (d-pad up would
    // close the very inventory it navigates). Keyboard keeps today's
    // toggles; the pad closes with B below.
    const auto keyPressedOnly = [&](InputAction action) {
        const platform::Key key = actionMap.binding(action).key;
        return key != platform::Key::Count && input.wasPressed(key);
    };

    // Tab: back/close the top screen (dev request 2026-07-07 — the
    // Skyrim reflex). Escape only drives the pause/main menus below.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() && !capturingBind &&
        input.wasPressed(platform::Key::Tab)) {
        screenStack.closeTop();
    }
    // Escape: toggle the pause menu in Play (and still dismiss the boot
    // main menu for the dev tools); Fly/Edit keep Escape free for tools.
    if (!imguiOwnsKeys && !capturingBind &&
        actionMap.pressed(input, InputAction::Pause)) {
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
    // C9.3: keyboard-only while a modal is open (pad binding = d-pad up,
    // which is navigation there).
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() &&
        (screenStack.modalOpen()
             ? keyPressedOnly(InputAction::Inventory)
             : actionMap.pressed(input, InputAction::Inventory))) {
        const ScreenStack::Screen* top = screenStack.topModal();
        if (top && (top->name == "inventory" || top->name == "container")) {
            screenStack.closeTop();
        } else if (!screenStack.modalOpen() && playerEntity.is_alive()) {
            uiRouter.openInventoryScreen(makeUiRouterContext());
        }
    }
    // T: the wait menu (B6) — Play only, nothing else open.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() && (mode == SceneMode::Play) &&
        actionMap.pressed(input, InputAction::WaitMenu) &&
        !screenStack.modalOpen()) {
        hud.updateMenuClockLine(makeHudContext());
        screenStack.show("wait");
    }
    // J: the quest journal (chantier 6 A3) — the I-key idiom.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() &&
        actionMap.pressed(input, InputAction::Journal)) {
        const ScreenStack::Screen* top = screenStack.topModal();
        if (top && top->name == "journal") {
            screenStack.closeTop();
        } else if (!screenStack.modalOpen()) {
            hud.pushJournalModel(makeHudContext());
            screenStack.show("journal");
        }
    }
    // M: the map (C9.6) — the I-key idiom, Play only (the raster shows
    // the world the PLAYER walks). Keyboard-only while a modal is open:
    // the pad binding is d-pad LEFT, which is navigation there.
    if (!imguiOwnsKeys && !uiSystem.textFieldFocused() &&
        (screenStack.modalOpen()
             ? keyPressedOnly(InputAction::Map)
             : actionMap.pressed(input, InputAction::Map))) {
        const ScreenStack::Screen* top = screenStack.topModal();
        if (top && top->name == "map") {
            screenStack.closeTop();
        } else if (!screenStack.modalOpen() && (mode == SceneMode::Play)) {
            mapController.open(makeMapContext());
        }
    }

    const bool modal = screenStack.modalOpen();
    const bool modalJustOpened = modal && !uiModalWasOpen;
    if (modal != uiModalWasOpen) {
        // A modal frees the mouse (and pauses the sim, handled in
        // update()); closing it restores the Play capture.
        if ((mode == SceneMode::Play)) {
            engine->getWindow().setRelativeMouseMode(!modal);
        }
        uiModalWasOpen = modal;
    }
    if (modal && !capturingBind) { // C9.4: a capture eats mouse/keys too
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

        // C9.3: pad -> UI. The d-pad and the left stick pulse the arrow
        // keys through RmlUi (its spatial navigation moves the focus,
        // exactly like keyboard arrows), A clicks the focused element,
        // B is the Tab reflex (close the top screen). Start already
        // works: the Pause action above fires from Escape OR Start.
        // Skipped on the opening frame so the pad edge that opened a
        // screen (d-pad up = inventory) does not also navigate it.
        if (!modalJustOpened) {
            using platform::PadButton;
            const auto pulse = [&](platform::Key key) {
                uiSystem.processKey(key, true);
                uiSystem.processKey(key, false);
            };
            if (input.padPressed(PadButton::DPadUp)) {
                pulse(platform::Key::Up);
            }
            if (input.padPressed(PadButton::DPadDown)) {
                pulse(platform::Key::Down);
            }
            if (input.padPressed(PadButton::DPadLeft)) {
                pulse(platform::Key::Left);
            }
            if (input.padPressed(PadButton::DPadRight)) {
                pulse(platform::Key::Right);
            }
            // Left stick: past the threshold it pulses the dominant
            // axis, then repeats while held (the menu-scroll feel);
            // recentering rearms an immediate pulse.
            constexpr f32 kStickNavThreshold = 0.6f;
            constexpr f32 kStickNavRepeat = 0.25f; // seconds between pulses
            const Vec2 stick = input.leftStick();
            uiStickCooldown -= dt;
            if (std::abs(stick.x) < kStickNavThreshold &&
                std::abs(stick.y) < kStickNavThreshold) {
                uiStickCooldown = 0.0f;
            } else if (uiStickCooldown <= 0.0f) {
                if (std::abs(stick.y) >= std::abs(stick.x)) {
                    pulse(stick.y > 0.0f ? platform::Key::Up
                                         : platform::Key::Down);
                } else {
                    pulse(stick.x > 0.0f ? platform::Key::Right
                                         : platform::Key::Left);
                }
                uiStickCooldown = kStickNavRepeat;
            }
            if (input.padPressed(PadButton::A)) {
                uiSystem.activateFocused();
                uiPadConsumedA = true;
            }
            if (input.padPressed(PadButton::B)) {
                screenStack.closeTop();
                uiPadConsumedB = true;
            }
        }
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
        // C9.6: the player marker follows while the map is up (a modal
        // pauses the sim, but travel fades / Escape-close paths keep the
        // marker honest on reopen anyway — this is one cheap deduped push).
        if (top && top->name == "map") {
            mapController.updateOpen(makeMapContext());
        }
    }

    hud.updateHudModel(makeHudContext());
    syncScreens();
    uiSystem.update(dt);

    // C9.3: give the pad somewhere to land. Whenever a modal is open
    // and no navigable element holds the focus — the screen just
    // opened, a data-for rebuild destroyed the focused row, a click
    // landed on dead space — focus the top screen's first (or
    // "selected") element. After update(): a freshly shown document
    // only has computed styles and its data-for rows once the context
    // updated.
    if (modal && !uiSystem.hasNavigableFocus()) {
        if (const ScreenStack::Screen* top = screenStack.topModal()) {
            uiSystem.focusFirst(top->document);
        }
    }
}

// Bundle the scene systems the RmlUi presenter reads for GameHud this frame
// — references into the scene plus a few scalars (audit U4-9). Rebuilt each
// call (cheap). Mirrors makeEditorContext / makeInteractionContext.
HudContext LandscapeScene::makeHudContext() {
    return HudContext {
        uiSystem,
        uiCreated,
        forms,
        texts, // C9.5: the C++-formatted ui.* strings
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
        playerController.bowCharge(), // A7+: the draw gauge
        statsTuning.hudStatPointsScale, // R7: vitals-bar scale
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
        texts, // C9.5: the save.saved toast
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
        &engine->getJobSystem(), // C9.7: serialize + write off the frame
    };
}

bool LandscapeScene::finalizeActorSpawn(ecs::Entity entity,
                                        const core::Guid& actorFormId) {
    const gameplay::CharacterTickContext tickCtx { derivedStats, gameTags,
                                                   statsTuning };
    core::Guid refGuid;
    if (entity.has<world::RefId>()) {
        refGuid = entity.get<world::RefId>().referenceId;
    }
    // The "was captured" sentinel, resolved ONCE and reused below: pending
    // layer first (a cell reloading in THIS session), then the resolved
    // database (a loaded save). A captured actor never re-rolls its
    // loadout (§8) NOR re-curves its attributes (É5) — saved bases win.
    const bool hasPendingState =
        saveController.pending().hasActorState(refGuid);
    const gameplay::SavedActorRecords saved =
        hasPendingState ? gameplay::SavedActorRecords {}
                        : gameplay::savedRecordsFor(forms, refGuid);
    const bool fromSave = hasPendingState || saved.stats != nullptr;
    // FOLLOWERS É5: a FRESH actor with a follower class draws his 9
    // attribute BASES from the class curves at his starting level, BEFORE
    // initializeActorStats derives the maxima from those bases and fills
    // vitals to full (right at spawn — level CHANGES go through the
    // vitals-preserving path in FollowerController instead). §2.9: this
    // is the sanctioned spawn-time init write (the É0 classAttributesAt
    // curves), gated by the same sentinel as the loadout re-roll.
    if (const data::FormHandle actorHandle =
            (!fromSave && actorFormId.isValid()) ? forms.handleOf(actorFormId)
                                                 : data::FormHandle {};
        actorHandle.isValid()) {
        const data::Form* baseForm = forms.get(actorHandle);
        const reflect::TypeInfo* formType = forms.typeOf(actorHandle);
        if (baseForm && formType &&
            formType->isA(data::ActorForm::staticTypeInfo().id)) {
            const auto* actor =
                static_cast<const data::ActorForm*>(baseForm);
            const auto* followerClass =
                forms.find<gameplay::FollowerClassForm>(
                    actor->followerClass);
            if (followerClass && entity.has<gameplay::CoreAttributes>()) {
                if (!entity.has<gameplay::FollowerState>()) {
                    entity.set<gameplay::FollowerState>({});
                }
                auto& state = entity.get_mut<gameplay::FollowerState>();
                // Doc §2: followers start at their authored minimum level.
                state.followerLevel = std::max(1.0f, actor->minLevel);
                gameplay::applyFollowerClass(
                    entity.get_mut<gameplay::CoreAttributes>(),
                    *followerClass, state.followerLevel);
                if (entity.has<gameplay::AttributeSet>()) {
                    // The level attribute mirrors FollowerState (É0 note).
                    entity.get_mut<gameplay::AttributeSet>().level =
                        state.followerLevel;
                }
            }
        }
    }
    gameplay::initializeActorStats(entity, tickCtx);
    // P0 A3: every actor can swing — the shared melee state machine
    // (player LMB and NPC AI go through the same component).
    if (!entity.has<gameplay::MeleeSwing>()) {
        entity.set<gameplay::MeleeSwing>({});
    }
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
    // FOLLOWERS É0: same rule — the follower* SavedStatsForm fields need
    // the component present before the saved-state apply to land.
    if (!entity.has<gameplay::FollowerState>()) {
        entity.set<gameplay::FollowerState>({});
    }
    // Re-apply captured instance overrides: a moved/killed actor keeps the
    // spot it died at instead of snapping back to its authored spawn (the
    // cell loader respawns the resolved record). finalize runs AFTER
    // refreshNpcs grounds the actor's Y, so the captured position wins.
    saveController.pending().applyReferenceOverrides(entity, refGuid);
    // The sentinel resolved at the top: a captured actor restores its
    // saved state instead of rolling a loadout (§8) or re-curving (É5).
    if (hasPendingState) {
        gameplay::applySavedState(
            entity, saveController.pending().actorState(refGuid), gameTags);
        return true;
    }
    if (saved.stats) {
        gameplay::applySavedState(entity, saved, gameTags);
        return true;
    }
    if (actorFormId.isValid()) {
        gameplay::applyLoadout(forms, actorFormId,
                               entity.get_mut<gameplay::Inventory>(),
                               lootRng);
        // The actor EQUIPS the first weapon its loadout rolled (the
        // player overrides this with the starting-kit equip): combat
        // stats AND the drawn model come from Equipment — the inventory
        // link the dev asked about (2026-07-11).
        auto& equipment = entity.get_mut<gameplay::Equipment>();
        if (!equipment.weapon.isValid()) {
            for (const auto& stack :
                 entity.get<gameplay::Inventory>().items) {
                if (forms.find<data::WeaponForm>(stack.item)) {
                    equipment.weapon = stack.item;
                    break;
                }
            }
        }
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
        texts, // C9.5: ui.* fallback strings
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
                slot, texts,
                [this](const str& m) { interaction.say(m, 3.0f); });
        },
        [this](f32 hours) {
            interaction.wait(hours, makeInteractionContext());
        },
        // Menu resume/leave go through THE transition (restoreMode owns
        // every mode side effect — dev rule 2026-07-10). restoreMode
        // no-ops when already in the mode, so resume-from-menu (mode
        // still Play) keeps its old direct-call behaviour via the else.
        [this] {
            if (mode == SceneMode::Play) {
                enterPlayMode(); // re-grab mouse/HUD after a modal menu
            } else {
                restoreMode(SceneMode::Play);
            }
        },
        [this] { restoreMode(SceneMode::Spectator); },
        [this] { engine->requestQuit(); },
        // C9.4: menuAction("options") — fresh values, then the screen.
        [this] { optionsController.open(makeOptionsContext()); },
    };
}

// C9.4: the options screen's slice — the settings + the ActionMap plus
// their live application points (input deadzone, the audio buses; C9.5
// adds the loc table and the live language application).
OptionsContext LandscapeScene::makeOptionsContext() {
    return OptionsContext { settings,    actionMap,   engine->getInput(),
                            uiSystem,    screenStack, audioSystem,
                            texts,       [this] { applyLanguage(); } };
}

// C9.6: the map screen's slice — resolved records + the terrain ground
// truth + which worldspace the player is in. The marker follows the Play
// capsule (the map only opens in Play mode; the Fly camera fallback keeps
// the context total anyway).
MapContext LandscapeScene::makeMapContext() {
    const Vec3 playerPos =
        (mode == SceneMode::Play) && playerController.body()
            ? playerController.body()->position()
            : flyCamera.camera.position;
    return MapContext { forms,
                        renderer.terrainParams(),
                        engine->getJobSystem(),
                        uiSystem,
                        screenStack,
                        activeWorldspace,
                        overworldHandle,
                        interiorMode,
                        playerPos };
}

// C9.5: the LIVE half of the language switch (the toggle already flipped
// and saved settings.language). Only the TextTable is rebuilt — it holds
// string COPIES, so no resolved Form pointer held by any controller moves
// (playerWeapon, effects... stay put; the full stack under `forms`
// re-resolves gated on the next scene enter as always). The re-gated
// stack resolves into a TEMPORARY database texts.build reads once; the
// data-loc pass then re-runs over every loaded document. C++-pushed model
// strings refresh on their next push (per frame for the HUD, on reopen
// for the screens; the options screen re-pushes right after this).
void LandscapeScene::applyLanguage() {
    const auto dataDir = platform::executableDir() / "data";
    const data::PluginConfig config = loadGatedPluginConfig(dataDir);
    const data::PluginStack stack =
        data::loadPluginStack(dataDir, config, formTypes);
    for (const str& error : stack.errors) {
        LOG_WARN("language switch: {}", error);
    }
    data::FormDatabase strings;
    data::resolve(data::pointersOf(stack), formTypes, strings);
    texts.build(strings);
    uiSystem.relocalize();
    LOG_INFO("Language '{}' applied live: {} strings", settings.language,
             texts.size());
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
            render::terrain::height(renderer.terrainParams(), position.x, position.z);
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
        const f32 y = render::terrain::height(renderer.terrainParams(), x, z) + 0.5f;
        if ((mode == SceneMode::Play) && playerController.body()) {
            playerController.spawnBody(*physics, Vec3 { x, y, z });
        } else {
            flyCamera.camera.position = { x, y + statsTuning.eyeHeight, z };
        }
        char out[64];
        std::snprintf(out, sizeof(out), "teleported to %.0f %.0f", x, z);
        return out;
    });
    panel.addCommand("fx", [this](const str& args) -> str {
        // P0 C1 dev bench: spawns a ParticleForm by editorId 4 m ahead
        // of the camera (grounded). `fx CampfireSmoke`
        const auto* form =
            data::findByEditorId<data::ParticleForm>(forms, args);
        if (!form) {
            return "no ParticleForm named '" + args + "'";
        }
        const render::Camera3D& cam = flyCamera.camera;
        Vec3 at = cam.position + cam.forward() * 4.0f;
        at.y = render::terrain::height(renderer.terrainParams(), at.x,
                                       at.z) +
               0.2f;
        // Cosmetic seed from the spot (§8: never the gameplay RNG).
        const u32 seed = static_cast<u32>(at.x * 73.0f) ^
                         (static_cast<u32>(at.z * 179.0f) << 8) ^
                         fxSim.count();
        fxSim.spawn(gameplay::toEmitterParams(*form), at, seed);
        return "fx '" + args + "' spawned (" +
               std::to_string(fxSim.count()) + " live, " +
               std::to_string(fxSim.emitterCount()) + " emitter(s))";
    });
    panel.addCommand("tgm", [this](const str&) -> str {
        return sceneConsole.toggleGodMode() ? "god mode ON" : "god mode OFF";
    });
    panel.addCommand("save", [this](const str& args) -> str {
        saveController.performSave(makeSaveContext(),
                                   args.empty() ? "quick" : args);
        // C9.7: the write is async — the toast + timing log land at
        // completion (pumpCompletions).
        return "saving '" + (args.empty() ? str { "quick" } : args) +
               "'...";
    });
    panel.addCommand("load", [this](const str& args) -> str {
        const str slot = args.empty() ? "quick" : args;
        if (!std::filesystem::exists(savePath(slot))) {
            return "no save named '" + slot + "'";
        }
        saveController.requestLoad(
            slot, texts,
            [this](const str& m) { interaction.say(m, 3.0f); });
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
    panel.addCommand("player.level", [this](const str& args) -> str {
        // FOLLOWERS É5 — the dev lever: player progression (skills-by-use)
        // is its own chantier, so until then the level only moves here.
        // §2.9: the sanctioned console/init base-write class (the tgm
        // registration pattern); the frame's tickCharacter recomputes the
        // currents, and the follower sweep (updateFollowers) syncs party
        // levels + the +1 attribute point off the new value.
        std::istringstream in { args };
        f32 level = 0.0f;
        if (!(in >> level) || level < 1.0f) {
            return "usage: player.level <n >= 1>";
        }
        if (!playerEntity.is_alive() ||
            !playerEntity.has<gameplay::AttributeSet>()) {
            return "no player";
        }
        playerEntity.get_mut<gameplay::AttributeSet>().level = level;
        char out[48];
        std::snprintf(out, sizeof(out), "player level = %.0f", level);
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
    // FOLLOWERS É4: the DIALOGUE PARTNER's follower state — affinity lives
    // on the follower being talked to, not on the player. Filled whenever
    // a partner with FollowerState is open, so dialogue options can gate
    // on FollowerAffinityAtLeast; everywhere else the clause fails closed.
    const ecs::Entity partner = questDirector.dialoguePartner();
    if (partner.is_alive() && partner.has<gameplay::FollowerState>()) {
        context.partnerFollower = &partner.get<gameplay::FollowerState>();
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
        texts, // C9.5: quest.* toast strings
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
        texts,
        sprintCostEffect,
        playerWeapon,
        attackAbility,
        dodgeAbility,
        npcDirector.npcs(),
        interaction,
        playerEncumbrance == gameplay::EncumbranceCategory::Overencumbered,
        [this] { questDirector.syncWantedTag(makeQuestContext()); },
        &eventBus, // C4a: synthesized player footsteps
        &fxDirector.cues(), // C2: hit/block/parry feedback
        // D2b: the water surface over a spot — sea level + the placed
        // volumes (the extract's snapshot copies, fresh this frame).
        [this](const Vec3& at) -> std::optional<f32> {
            std::optional<f32> best;
            if (!interiorMode) {
                const f32 sea = renderer.terrainParams().seaLevel;
                if (at.y < sea + 2.0f) {
                    best = sea;
                }
            }
            for (const auto& volume : snapshot.waterVolumes) {
                if (std::abs(at.x - volume.position.x) <=
                        volume.halfExtents.x &&
                    std::abs(at.z - volume.position.z) <=
                        volume.halfExtents.z) {
                    const f32 top =
                        volume.position.y + 2.0f * volume.halfExtents.y;
                    if (at.y < top + 0.5f && (!best || top > *best)) {
                        best = top;
                    }
                }
            }
            return best;
        },
        swimCostEffect, // D2b: §2.9 — only effects move energy
        sneakCostEffect,
        &projectileDirector, // A7: the bow
        bowDrawCostEffect,   // A7+: the drawn-bow drain
        &actionMap, // C9.2: intentions, not raw keys
        &settings,  // C9.2: look feel (sens/invert/stick)
    };
}

// --- B6: Forms-driven NPCs (NpcDirector, audit U4-10) -----------------------

// Bundle the NPC subsystem's dependencies for the director this call —
// references into the scene plus the player / weapon / clock scalars.
// Rebuilt each call (cheap: refs + scalars). U4-2b: no GPU handles anymore —
// the director's draw side moved behind the snapshot seam.
NpcContext LandscapeScene::makeNpcContext() {
    return NpcContext {
        world,
        forms,
        assetDb,
        renderer.terrainParams(),
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
        attackAbility,
        combatRng,
        sceneConsole.vm(), // brain scripts (docs/BOSS-SCRIPTING.md)
        &fxDirector.cues(), // C2: hit/block/parry/death feedback
        &projectileDirector, // A7: archer NPCs
        sceneConsole.godMode(),
        timeSeconds,
        &spatialIndex, // R3: radius queries (faction shout) share it
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
    // FOLLOWERS É1: owned tags are not captured actor state — re-mirror
    // Follower.Active onto the player after every spawn/reload sweep so
    // the dialogue conditions stay truthful across F9 and streaming.
    followerController.syncActiveTag(makeFollowerContext());
}

// FOLLOWERS É1: bundle the systems recruit/dismiss touch. Rebuilt per call
// (cheap). Mirrors the other make*Context builders.
FollowerContext LandscapeScene::makeFollowerContext() {
    return FollowerContext {
        world,
        forms,
        gameTags,
        statsTuning,
        renderer.terrainParams(),
        cellLoader.get(),
        saveController.pending(),
        playerEntity,
        npcDirector,
        // É3: bleedout/convalescence — game clock stamps, the seeded
        // combat RNG (§8), and the HUD toast for the loc'd feedback.
        gameClock,
        combatRng,
        &texts,
        [this](str line) { interaction.say(std::move(line), 4.0f); },
        // É4: the recruit-preview screen (model push + show).
        &uiSystem,
        &screenStack,
    };
}

void LandscapeScene::updateNpcs(f32 dt) {
    npcDirector.update(dt, makeNpcContext());
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
        renderer.terrainParams(),
        physics.get(),
        meshCache.get(),
        navigator.get(),
        focus,
        /*fastCook=*/interaction.fading() || interaction.fadeAlpha() > 0.0f,
        /*editorOwnsTransforms=*/mode == SceneMode::Edit,
    };
}

// The whole frame is the renderer's (audit U4-2c): the scene only assembles
// the per-frame VIEW — what the sim decided this frame — and hands over the
// snapshot. No render state, no GPU handle, no pass lives here anymore.
void LandscapeScene::render(engine::FrameContext& frame) {
    const phys::CharacterBody* playerBody = playerController.body();
    const RenderView view {
        .camera = flyCamera.camera,
        .atmos = atmos,
        .interiorMode = interiorMode,
        .timeSeconds = timeSeconds,
        .windTime = windTime,
        .snowLine = tuning.snowLine,
        .splatUvScale = tuning.splatUvScale,
        .interiorAmbient = tuning.interiorAmbient,
        .grassBend = (mode == SceneMode::Play) && playerBody != nullptr,
        .playerFeet = playerBody ? playerBody->position() : Vec3 { 0.0f },
        .meshCache = meshCache.get(),
        .materialTextures = materialTextures.get(),
        .gameUi = uiCreated ? &uiSystem : nullptr,
        .probe = &frameProbe,
    };
    renderer.render(frame, snapshot, view);
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

    // Mode hotkeys. Play is home: F2 toggles Play<->Spectator, F3 toggles
    // Play<->Edit, and from any OTHER mode the key REPLACES it with its
    // own (dev rule 2026-07-10). All side effects live in restoreMode —
    // the keys only pick the target. Both are inert while a modal menu
    // owns the input. F2/F3 (not F11/F12) stay clear of the CLion/VS
    // debugger's global Step Into/Over shortcuts.
    if (!screenStack.modalOpen()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
            restoreMode(mode == SceneMode::Spectator ? SceneMode::Play
                                                     : SceneMode::Spectator);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false) && levelEditor) {
            restoreMode(mode == SceneMode::Edit ? SceneMode::Play
                                                : SceneMode::Edit);
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
            "quick", texts,
            [this](const str& m) { interaction.say(m, 3.0f); });
    }

    // The dev console lives on ` (grave, left of 1 — the PC convention):
    // the ONLY dev UI allowed in Play mode, drawn as a full-width bottom
    // strip (SceneConsole::draw).
    if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
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

    // F10 hides/shows the whole dev UI (photo mode; the console stays on
    // its own key).
    if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) {
        uiPanelVisible = !uiPanelVisible;
    }
    if (!uiPanelVisible) {
        return;
    }

    // PLAY: no panels, no bar — the game (interfaces-par-mode decision).
    if (mode == SceneMode::Play) {
        return;
    }

    // Spectator/Edit: a thin full-width TOP BAR — status on the left,
    // window toggles on the right side of it. The windows it opens dock
    // on the RIGHT edge (movable afterwards). (`display` was captured at
    // the top of drawUi for the HUD/fade drawing.)
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(display.x, 0.0f));
    ImGui::Begin("##topbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoDocking);
    ImGui::TextUnformatted(mode == SceneMode::Edit ? "EDIT" : "SPECTATOR");
    ImGui::SameLine();
    ImGui::TextDisabled("| F2 spectator  F3 edit  ` console  F10 hide UI");
    ImGui::SameLine();
    ImGui::Text("| %.1f FPS", ImGui::GetIO().Framerate);
    const Vec3 p = flyCamera.camera.position;
    ImGui::SameLine();
    ImGui::TextDisabled("| %.0f %.0f %.0f", p.x, p.y, p.z);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::SliderFloat("##flyspeed", &flyCamera.moveSpeed, 2.0f, 150.0f,
                       "fly %.0f m/s", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::TextDisabled("|");

    // Toggle buttons: lit while their window is open. (Push/pop pairs on
    // the PRE-click state — the click may flip `open` mid-frame.)
    const auto barToggle = [](const char* label, bool& open) {
        ImGui::SameLine();
        const bool lit = open;
        if (lit) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImVec4(0.26f, 0.46f, 0.75f, 1.0f));
        }
        if (ImGui::SmallButton(label)) {
            open = !open;
        }
        if (lit) {
            ImGui::PopStyleColor();
        }
    };
    if (mode == SceneMode::Spectator) {
        // Landscape + graphics performance (dev decision).
        barToggle("Terrain", uiTerrainOpen);
        barToggle("Sky & weather", uiSkyOpen);
        barToggle("Rendering", uiRenderOpen);
        barToggle("GPU perf", uiPerfOpen);
    } else { // Edit: editing-related (the SceneEditor windows draw on
             // their own; these are the extras).
        barToggle("Gameplay", uiGameplayOpen);
        barToggle("Terrain", uiTerrainOpen);
        barToggle("Sky & weather", uiSkyOpen);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(sceneConsole.visible() ? "Console*"
                                                  : "Console")) {
        sceneConsole.toggle(false);
    }
    const f32 barBottom = ImGui::GetWindowSize().y;
    ImGui::End();

    // The right-docked windows the bar toggles.
    u32 rightSlot = 0;
    const auto rightWindow = [&](const char* title, bool& open,
                                 auto&& body) {
        if (!open) {
            return;
        }
        ImGui::SetNextWindowPos(
            ImVec2(display.x - 450.0f,
                   barBottom + 8.0f + 32.0f * static_cast<f32>(rightSlot)),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(440.0f, display.y * 0.55f),
                                 ImGuiCond_FirstUseEver);
        ++rightSlot;
        if (ImGui::Begin(title, &open)) {
            body();
        }
        ImGui::End();
    };
    rightWindow("Gameplay — player, NPC, physics", uiGameplayOpen,
                [&] { drawGameplayUi(); });
    rightWindow("Terrain & streaming", uiTerrainOpen,
                [&] { renderer.drawTerrainPanel(); });
    rightWindow("Sky, weather & time", uiSkyOpen, [&] { drawSkyUi(); });
    rightWindow("Rendering & post-FX", uiRenderOpen,
                [&] { renderer.drawRenderPanel(atmos); });
    rightWindow("GPU perf", uiPerfOpen,
                [&] { renderer.drawPerfPanel(&frameProbe); });

    // Edit mode: the scene switcher — a small vertical strip docked
    // middle-left. Overlays PUSH onto the SceneStack: the world below
    // pauses but stays WARM (no reload when popping back — the reason
    // overlay won over scene-unload).
    if (mode == SceneMode::Edit && host()) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, display.y * 0.35f),
                                ImGuiCond_FirstUseEver);
        ImGui::Begin("Scenes", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextDisabled("overlay\n(world pauses)");
        if (ImGui::Button("Game DB", ImVec2(110.0f, 0.0f))) {
            host()->push(std::make_unique<EditorScene>(*engine));
        }
        ImGui::End();
    }
}

void LandscapeScene::drawSkyUi() {
    if (ImGui::CollapsingHeader("Time", ImGuiTreeNodeFlags_DefaultOpen)) {
        // The clock is the source of truth: the slider WRITES it (day
        // count preserved), the sky follows in update().
        f32 hour = static_cast<f32>(std::fmod(gameClock.gameHours(), 24.0));
        if (ImGui::SliderFloat("Time of day (h)", &hour, 0.0f, 24.0f,
                               "%.1f")) {
            const f64 days = std::floor(gameClock.gameHours() / 24.0);
            gameClock.gameSeconds = (days * 24.0 + hour) * 3600.0;
        }
        ImGui::Checkbox("Animate (24 h in 2 min)", &animateTime);
        ImGui::SameLine();
        ImGui::TextDisabled("day %d, x%.0f",
                            static_cast<int>(gameClock.gameDays()),
                            gameClock.timescale);
    }
    if (!weather.states().empty() &&
        ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    }
    if (!weather.states().empty() &&
        ImGui::CollapsingHeader("Light & color")) {
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
    if (physics &&
        ImGui::CollapsingHeader("Player", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    }
    if (!npcDirector.npcs().empty() &&
        ImGui::CollapsingHeader("NPC", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    }
    if (physics && ImGui::CollapsingHeader("Physics debug")) {
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

} // namespace game
