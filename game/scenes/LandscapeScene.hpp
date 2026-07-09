#pragma once

#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "engine/core/Rng.hpp"
#include "engine/anim/Anim.hpp"
#include "engine/assets/AssetDatabase.hpp"
#include "engine/assets/GltfMesh.hpp"
#include "engine/ecs/World.hpp"
#include "engine/render/FlyCamera.hpp"
#include "engine/render/ShaderLibrary.hpp"
#include "engine/physics/Physics.hpp"
#include "game/LevelEditor.hpp"
#include "game/MeshCache.hpp"
#include "game/scenes/GameHud.hpp"
#include "game/scenes/InteractionController.hpp"
#include "game/scenes/SceneEditor.hpp"
#include "game/scenes/StreamingController.hpp"
#include "game/scenes/NpcDirector.hpp"
#include "game/scenes/PlayerController.hpp"
#include "game/scenes/AtmosphereParams.hpp"
#include "game/scenes/WeatherController.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/ai/ScheduleSystem.hpp"
#include "gameplay/condition/Condition.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/interaction/Furniture.hpp"
#include "gameplay/save/SaveState.hpp"
#include "engine/core/FrameProbe.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "quest/Dialogue.hpp"
#include "quest/Quest.hpp"
#include "engine/ui/UiSystem.hpp"
#include "game/InventoryView.hpp"
#include "game/SaveGame.hpp"
#include "game/ScreenStack.hpp"
#include "world/ai/TerrainNavigator.hpp"
#include "world/streaming/CellStreamer.hpp"
#include "game/SceneSubmit.hpp"
#include "game/TerrainCollision.hpp"
#include "game/TextureCache.hpp"
#include "game/VegetationCollision.hpp"
#include "game/scenes/LandscapeTuning.hpp"
#include "engine/render/landscape/ChunkOcclusion.hpp"
#include "engine/render/landscape/GpuOcclusion.hpp"
#include "engine/render/landscape/TerrainLightMap.hpp"
#include "engine/render/landscape/GrassSystem.hpp"
#include "engine/render/landscape/PostFx.hpp"
#include "engine/render/landscape/ShadowMapper.hpp"
#include "engine/render/landscape/SkySystem.hpp"
#include "engine/render/landscape/TerrainSystem.hpp"
#include "engine/render/landscape/VegetationSystem.hpp"
#include "engine/render/landscape/WaterSystem.hpp"
#include "engine/rhi/Rhi.hpp"
#include "game/Scene.hpp"

namespace engine {
class Engine;
}
namespace data {
struct WeaponForm;   // CoreForms — pointers only in this header
struct MiscItemForm; // (gold, chantier 4 B5)
class EditSession;   // console (chantier 4 B7)
}
namespace script {
class Vm;
}

namespace game {

class ConsolePanel;

// The 3D landscape renderer prototype (custom-renderer path, Phases 11-14).
// Owns the frame: records its own render passes instead of the sprite path.
// Brick 21: bloom (soft-threshold HDR pyramid, additive upsample) and
// screen-space god rays (radial march toward the sun over sky-only
// radiance), both composed in linear HDR by the tonemap pass.
class LandscapeScene final : public Scene {
public:
    explicit LandscapeScene(engine::Engine& engineContext)
        : engine(&engineContext) {}

    void onEnter() override;
    void onExit() override;

    void update(f32 dt) override;

    bool ownsFrame() const override { return true; }
    void render(engine::FrameContext& frame) override;
    void drawUi() override;

private:
    // Themed panel sections (drawUi wraps them in collapsing headers).
    void drawGameplayUi();
    void drawSkyUi();
    void drawRenderUi();

    // Offscreen color+depth target at window size, recreated on resize.
    void ensureOffscreenTarget(rhi::Device& device, u32 width, u32 height);
    void destroyOffscreenTarget(rhi::Device& device);
    void rebuildBlitPipeline(rhi::Device& device);

    engine::Engine* engine { nullptr };

    // Moddable startup values (§5): loaded from data/base/landscape.toml
    // (plus any mod patches) in onEnter, then copied into the systems' plain
    // params and the UI members below — the panel still adjusts everything
    // live; the TOML sets where it all starts.
    data::FormTypeRegistry formTypes;
    LandscapeTuningForm tuning;

    // Weather (brick 24, extracted to WeatherController brick 3b): precreated
    // states from landscape.toml, crossfaded into `atmos` over its duration.
    // The blend writes the same fields the sliders edit, so the panel shows
    // live values and manual tweaking resumes once the transition lands.
    WeatherController weather;

    render::FlyCamera flyCamera;
    f32 timeSeconds { 0.0f };

    // Panel layout: themed collapsing sections, toggled by click or F-key
    // (F1-F4 via ImGui's own key state — no platform::Key extension);
    // F10 hides the whole panel (screenshots, immersion).
    bool uiPanelVisible { true };
    bool uiGameplayOpen { true };
    bool uiTerrainOpen { false };
    bool uiSkyOpen { false };
    bool uiRenderOpen { false };

    uptr<render::ShaderLibrary> shaders;
    render::TerrainSystem terrain;
    render::GrassSystem grass;
    render::VegetationSystem vegetation;
    render::SkySystem sky;
    render::ChunkOcclusion occlusion;
    bool occlusionUi { true }; // height-horizon occlusion culling (A/B)
    render::GpuOcclusion gpuOcclusion;
    bool gpuOcclusionUi { true }; // Hi-Z compute culling (A/B)
    std::unordered_set<u64> gpuOccluded;      // last frame's GPU verdict
    std::unordered_set<u64> combinedOccluded; // CPU horizon ∪ GPU Hi-Z
    vector<render::TerrainSystem::ChunkAabb> occlusionAabbs;
    vector<render::GpuOcclusion::Candidate> occlusionCandidates;
    render::ShadowMapper shadows;
    render::WaterSystem water;
    render::PostFx postFx;
    bool regenerateRequested { false };
    bool wireframeUi { false };
    bool animateTime { false };
    bool tonemapUi { true };
    bool stylizedUi { true }; // BotW step lighting vs classic wrap (A/B)
    bool shadowsUi { true };
    bool cascadeDebugUi { false };
    bool reflectionsUi { true };
    // The hysteresis-quantized sun the shadow cascades follow (a
    // continuously rotating light re-bases the texel snap every frame —
    // crawling edges); lighting keeps the smooth skyState sun.
    Vec3 shadowSunDirection { 0.0f, 1.0f, 0.0f };
    // Chantier 6 B3 (brick 28): analytical grade — OFF by default, the
    // dev A/Bs it before it can change the (liked) exterior look.
    bool gradingUi { false };
    f32 gradeVibranceUi { 0.3f };
    f32 gradeSplitToneUi { 0.35f };
    f32 gradeContrastUi { 1.06f };
    // Chantier 6 B4 (brick 29): auto-exposure — OFF by default (A/B).
    bool autoExposureUi { false };
    f32 autoExposureMinUi { 0.4f };
    f32 autoExposureMaxUi { 2.5f };
    f32 exposureUi { 1.0f };
    // Atmospheric render state (sky/fog/weather-driven), grouped so the weather
    // transition can own it (brick 3a). Manual sliders and the crossfade both
    // write here; render() reads it. stormFront/rainIntensity live here too
    // (were separate `*Ui` members near the storm/rain resources).
    AtmosphereParams atmos;
    f32 ssaoUi { 0.7f };
    i32 debugBufferUi { 0 }; // 0 off, 1 bloom, 2 god rays, 3 vol, 4 ssao
    f32 windTime { 0.0f }; // accumulated wind phase (dt x strength)

    rhi::BufferHandle frameUbo {};
    rhi::BindGroupHandle frameBindGroup {};
    // Chantier 2 B5: local lights UBO (binding 5, same group as FrameUbo).
    // The 16 nearest LightSource entities, flicker applied CPU-side.
    rhi::BufferHandle lightsUbo {};
    static constexpr u32 kMaxLights = 16;

    // B1 (chantier 1): the real mesh path replacing the H8 hardcoded cube.
    // A small ECS world spawned from plugin ReferenceForms; extractMeshes
    // fills the snapshot each frame; the residency caches resolve guids to
    // GPU resources (placeholders while pending — never block, §7).
    data::FormDatabase forms;      // resolved plugin stack (member: material
                                   //   lookups happen at draw time)
    data::PluginStack pluginStack; // owns the plugins behind `forms`
    assets::AssetDatabase assetDb; // guid -> file, layered per plugin order
    ecs::World world;
    // Cached flecs queries for the PER-FRAME paths (creating a query is an
    // allocation + registry insert — never per frame). Handles into
    // `world`; rebuilt right after it in onEnter.
    flecs::query<const world::Transform, const world::DoorTarget> doorQuery;
    flecs::query<const world::Transform, const world::RefId> interactQuery;

    // Chantier 2 B1: cells stream around the player (synchronous ring —
    // async + persistence is the « persistance » chantier). References
    // with no cell are persistent (the player), spawned once at enter.
    world::FormCategoryRegistry categories; // must outlive the CellLoader
    world::Spawner spawner;
    world::WorldModel worldModel;
    uptr<world::CellLoader> cellLoader;
    uptr<world::CellStreamer> cellStreamer;
    data::FormHandle overworldHandle {};

    // Chantier 2 B7: worldspace travel through doors. `activeWorldspace`
    // drives the streamer; `interiorMode` reshapes the renderer (no
    // terrain/sky/sun/water — ambient + local lights only).
    data::FormHandle activeWorldspace {};
    bool interiorMode { false };

    // In-game interaction mode. Play = first-person capsule; Spectator = free
    // fly camera that pauses the sim (the base for a future photo mode); Edit =
    // the level editor over the world. Replaces the former playMode/editMode
    // bools, making the states mutually exclusive; transitions go through
    // enter/exitPlayMode and the mode hotkeys. Target (see docs): the editor
    // becomes a stacked SceneStack layer over the running game.
    enum class SceneMode { Spectator, Play, Edit };
    SceneMode mode { SceneMode::Spectator };
    // The gameplay mode to return to when a menu (pause/main) closes on Escape.
    // Defaults to Play so a fresh boot lands in Play, and tracks the last mode
    // the player was actively in (updated each frame outside menus).
    SceneMode lastActiveMode { SceneMode::Play };

    // Chantier 2 B3/B4 + B9: level editor + terrain sculpt, extracted to
    // SceneEditor (audit U4-5). The scene owns the EditSession (levelEditor)
    // and the systems; SceneEditor owns the editor STATE (selection, palette,
    // gizmo, sculpt tool) and the interaction/UI. Wired each frame through
    // EditorContext (makeEditorContext, which folds in the sculpt sub-contract
    // makeSculptContext). Target: SceneEditor becomes a stacked SceneStack layer.
    uptr<LevelEditor> levelEditor;
    SceneEditor sceneEditor;
    EditorContext makeEditorContext();
    SculptContext makeSculptContext();
    // Chantier 3 B1: GENERIC interaction (E) + travel fade + talk toast,
    // extracted to InteractionController (audit U4-10). performTravel STAYS
    // here (a worldspace swap is streaming/scene territory — cellStreamer,
    // colliders, NPC refresh, player capsule); the controller fires it
    // through the InteractionContext travel callback at the black of the
    // fade. Wired each call through makeInteractionContext().
    InteractionController interaction;
    InteractionContext makeInteractionContext();
    void performTravel(const core::Guid& targetReference);

    // Chantier 3 B1: the game clock owns time-of-day (the sky follows)
    // and feeds real game-time into tickCharacter/schedules.
    gameplay::GameClock gameClock;

    // Chantier 4 B2: the RmlUi game UI. Screens come from UiScreenForm
    // records (documents through the plugins' ui/ roots — the SkyUI
    // model); the ScreenStack decides what is visible, a modal screen
    // pauses the sim and owns mouse/keyboard. Dev panels stay ImGui.
    ::ui::UiSystem uiSystem; // ::ui — game::ui (panels) masks it
    ScreenStack screenStack;
    bool uiCreated { false };
    bool uiModalWasOpen { false };
    bool uiTextInputOn { false };
    vector<str> shownScreens; // documents currently shown (sync state)
    // onEnter phases (brick U4-3): onEnter() runs these in order. Split for
    // readability only — behaviour is identical to the former 620-line body.
    void bootstrapData();                             // plugins/save/tuning
    void createRenderResources(rhi::Device& device);  // GPU resources + game UI
    void setupGameplay();                             // physics/nav/stats/quest
    void setupWorldAndStreaming();                    // ECS world, cells, clock
    void spawnInitialWorld(rhi::Device& device);      // spawn + npc + camera

    void createGameUi(rhi::Device& device);
    void updateGameUi(f32 dt);
    void syncScreens();
    vector<const ScreenStack::Screen*> screenStackPreloadList() const;

    // Audit U4-9: every push*Model / update*Model (game state -> UiSystem
    // data models) plus the view-model state (InventoryViews, dialogue
    // options) lives in GameHud, wired per call through makeHudContext().
    // Game ACTIONS stay here (handleUiEvent/handleMenuAction, equip/use/
    // transfer/barter, open*Screen) and mutate the views via hud accessors.
    GameHud hud;
    HudContext makeHudContext();

    // Chantier 4 B3: inventory + container/loot (SkyUI table logic in
    // InventoryView; the same component serves the barter screen, B5).
    ecs::Entity containerEntity {};
    void openInventoryScreen();
    void openContainerScreen(ecs::Entity container);
    void handleUiEvent(const str& model, const str& event,
                       const vector<str>& args);
    void toggleEquip(const core::Guid& id);
    void useConsumable(const core::Guid& id);
    void transferItem(const core::Guid& id, bool fromContainer);

    // Chantier 6 A2: the quest log (scene-level, like the clock) + the
    // demo quest. Quest state mirrors into PLAYER tags (Phase-4 pattern)
    // so dialogue options gate on it through the condition evaluator.
    quest::QuestLog questLog;
    const quest::QuestForm* easternQuest { nullptr };
    void syncQuestTags();
    void handleQuestEvent(const gameplay::Event& event);

    // Chantier 6 D2 — crime v1: assault on a peaceful NPC in front of a
    // witness (LOS) = bounty on the reflected Bounty component, mirrored
    // into the Crime.Wanted tag (conditions can't see components).
    void syncWantedTag();

    // Chantier 4 B4: dialogue — the Phase-4 tree + condition evaluator,
    // surfaced by the RmlUi screen.
    gameplay::EventBus eventBus;
    uptr<quest::DialogueRunner> dialogueRunner;
    ecs::Entity dialoguePartner {}; // who [E] Talk opened (vendor for B5)
    gameplay::EvalContext makeEvalContext() const;
    void openDialogue(const core::Guid& dialogueId);

    // Chantier 4 B5: barter. Gold is an ordinary item; prices = goldValue
    // × the StatsTuningForm multipliers; the vendor's stock/wealth is its
    // LoadoutEntryForm-rolled Inventory (limited — no restock yet).
    bool barterMode { false };
    core::Rng lootRng { 0x4d7a9b30u }; // loadout rolls (§8 seeded)
    const data::MiscItemForm* goldForm { nullptr };
    // D1: the OPEN vendor's effective multipliers (ActorForm override or
    // the global tuning), captured by openBarterScreen.
    f32 vendorBuyMult { 1.5f };
    f32 vendorSellMult { 0.5f };
    void openBarterScreen(ecs::Entity vendor);
    void barterTrade(const core::Guid& item, bool playerBuys);

    // Chantier 4 B6: menus (pause / main / wait / workstation screens
    // share one "menu" data model — the documents differ, the actions
    // route through handleMenuAction).
    void handleMenuAction(const str& action);

    // Chantier 5 B3: the one post-spawn seam for EVERY actor (player and
    // NPC): stat init, then saved state (when this actor was captured —
    // its SavedStatsForm is the sentinel) or the data loadout. Returns
    // true when saved state applied (fresh-game extras skip then).
    bool finalizeActorSpawn(ecs::Entity entity,
                            const core::Guid& actorFormId);

    // Chantier 5 B4: the pending in-memory layer — the memory of unloaded
    // cells (looted crates stay looted without a disk save). Hooked into
    // CellLoader (beforeUnload capture, spawnFilter veto) each onEnter.
    PendingSaveLayer pendingSave;

    // Chantier 5 B5: disk saves. performSave captures everything live +
    // flushes the pending layer into one ordinary plugin (§5) written to
    // saves/<slot>.toml. Loading re-enters the scene with the save
    // resolved as the LAST layer; the WorldStateForm restores clock/
    // worldspace/camera and skips the boot main menu.
    str pendingLoadSlot;     // consumed by the next onEnter
    bool reloadRequested { false }; // exit+enter at the end of update()
    bool loadedFromSave { false };  // this session came from a save file
    std::optional<gameplay::WorldStateForm> loadedWorldState;
    void performSave(const str& slot);
    void requestLoad(const str& slot);

    // Chantier 4 B7: dev console in the game scene (F8) — the H2 panel
    // with world commands registered on top (spawn/tp/tgm/settime), plus
    // god mode and the nameplates over hostile/hurt NPCs.
    uptr<data::EditSession> consoleSession;
    uptr<script::Vm> consoleVm;
    uptr<ConsolePanel> console;
    bool consoleVisible { false };
    bool godMode { false };
    void createConsole();

    // Chantier 3 B5/B6: melee combat — everything flows through the GAS
    // damage pipeline (weaponDamageEvent -> applyDamage), like the 2D
    // CombatArena. First-person: no player swing anim needed in v1.
    const data::WeaponForm* playerWeapon { nullptr };
    const data::WeaponForm* banditWeapon { nullptr };

    // Chantier 2 B8: the authored-terrain overlay. IMMUTABLE once
    // published; the sculpt tool edits a working copy then publishes a
    // NEW instance. Lifetime is carried by TerrainParams.patches itself
    // (shared_ptr — worker-held copies keep old instances alive, even
    // across scene teardown).
    sptr<const render::HeightPatches> heightPatches;
    // Chunks a sculpt changed, awaiting a TARGETED GPU rebuild at the safe
    // point in render() — so a stroke touches just those chunks, not the whole
    // world (keys use keyOf()). `sculptDirtyChunks` re-meshes the terrain (live
    // during a stroke, seamless); `sculptScatterChunks` re-scatters grass/veg
    // (on commit only — re-seeding every preview frame would flicker them).
    vector<u64> sculptDirtyChunks;
    vector<u64> sculptScatterChunks;
    uptr<TextureCache> materialTextures; // SRGBA8 + Linear (3D albedo)
    uptr<MeshCache> meshCache;
    RenderSnapshot snapshot;
    rhi::TextureHandle whiteTexture {}; // albedoTexture = 0 -> plain tint
    rhi::SamplerHandle meshSampler {};
    // Per-snapshot-entry GPU state (tiny N; instancing per model+material
    // is the planned next step of the contract — HORIZONTAL-PASS note).
    struct MeshDraw {
        rhi::BufferHandle ubo {};
        rhi::BindGroupHandle group {};
        rhi::TextureHandle boundTexture {};
        core::Guid material {};
        rhi::BindGroupHandle casterGroup {}; // B2a: ubo at binding 4
    };
    vector<MeshDraw> meshDraws;
    rhi::PipelineHandle meshPipeline {};
    u64 meshShaderGeneration { 0 };
    void buildMeshPipeline(rhi::Device& device);
    void drawSceneMeshes(engine::FrameContext& frame);

    // Brick 34 (chantier 7.1): dust light shafts — one small additive
    // blade-prism per shaft light, rebuilt when its direction moves
    // (sun-linked shafts follow the quantized shadow sun).
    struct LightShaft {
        u64 entityId { 0 };
        bool seen { false }; // mark/sweep against unloaded cells
        rhi::BufferHandle vertices {};
        rhi::BufferHandle ubo {};
        rhi::BindGroupHandle group {};
        Vec3 cachedDir { 0.0f };
        u32 vertexCount { 0 };
    };
    vector<LightShaft> lightShafts;
    rhi::PipelineHandle shaftPipeline {};
    u64 shaftShaderGeneration { 0 };
    // Brick 32 (chantier 7.4): placed water volumes — one alpha-blended
    // surface quad per volume + the camera-inside test feeding the
    // tonemap submersion.
    struct WaterQuad {
        u64 entityId { 0 };
        bool seen { false };
        rhi::BufferHandle vertices {};
        rhi::BufferHandle ubo {};
        rhi::BindGroupHandle group {};
    };
    vector<WaterQuad> waterQuads;
    rhi::PipelineHandle waterVolumePipeline {};
    u64 waterVolumeShaderGeneration { 0 };
    void drawWaterVolumes(engine::FrameContext& frame);
    f32 effectiveWaterSurfaceY() const; // brick 32 submersion input
    // Brick 30 (chantier 7.6): horizon cumulonimbus — a static buffer of
    // 8 camera-anchored towers, visible only while stormFront > 0.
    rhi::BufferHandle stormVertices {};
    rhi::PipelineHandle stormPipeline {};
    u64 stormShaderGeneration { 0 };
    // Brick 31 (chantier 7.7): procedural rain streaks + the top-down
    // occlusion depth (no rain under roofs) + global wetness.
    rhi::PipelineHandle rainPipeline {};
    u64 rainShaderGeneration { 0 };
    rhi::TextureHandle rainOcclusionTex {};
    rhi::FramebufferHandle rainOcclusionFb {};
    rhi::SamplerHandle rainSampler {};
    rhi::BufferHandle rainOcclusionUbo {};
    rhi::BindGroupHandle rainCasterGroup {};
    rhi::BindGroupHandle rainReceiverGroup {};
    bool shaftsUi { true };
    bool contactShadowsUi { true }; // brick 33a
    // Brick 33b/c: worker-baked terrain sun-shadow + sky-openness map.
    render::TerrainLightMap terrainLightMap;
    bool terrainLightUi { true };
    void buildShaftPipeline(rhi::Device& device);
    void drawLightShafts(engine::FrameContext& frame,
                         const Vec3& sunColor);

    // Chantier 6 B2a: meshes + skinned NPCs cast into the sun cascades
    // (depth-only pipelines; the model UBOs are re-used, one frame behind
    // for NPCs — invisible at shadow resolution). Toggle = the A/B guard.
    rhi::PipelineHandle meshCasterPipeline {};
    rhi::PipelineHandle skinnedCasterPipeline {};
    u64 meshCasterShaderGeneration { 0 };
    u64 skinnedCasterShaderGeneration { 0 };
    bool meshShadowCastersUi { true };
    void buildCasterPipelines(rhi::Device& device);
    void drawShadowCasters(engine::FrameContext& frame, u32 cascade);
    void drawCastersInto(engine::FrameContext& frame,
                         rhi::BindGroupHandle casterGroup, bool refreshUbos);
    // B2b (chantier 7.5): the interior key-light shadow — ONE perspective
    // depth layer from the castsShadow light nearest the camera; stops a
    // candle from lighting through a wall. Never touches kCascadeCount.
    rhi::TextureHandle keyShadowTex {};
    rhi::FramebufferHandle keyShadowFb {};
    rhi::SamplerHandle keyShadowSampler {};
    rhi::BufferHandle keyShadowUbo {};
    rhi::BindGroupHandle keyShadowCasterGroup {};
    rhi::BindGroupHandle keyShadowReceiverGroup {};
    bool keyShadowUi { true };

    // B6 (chantier 1): Forms-driven skinned NPCs — the whole subsystem
    // (rig cache, NPC list, build/AI/schedule/combat/draw) lives in
    // NpcDirector (audit U4-10), behind an NpcContext the scene builds each
    // call. The scene keeps only cross-cutting reads via npcDirector.npcs()
    // (player attack/crime, shadow caster pass, debug UI, editor, console).
    NpcDirector npcDirector;
    NpcContext makeNpcContext();
    // Thin delegators kept so the many call sites stay unchanged; each just
    // bundles the context and forwards to the director.
    void refreshNpcs(rhi::Device& device);
    void updateNpcs(f32 dt);
    void drawNpcs(engine::FrameContext& frame);

    // Chantier 3 B2/B3: navigation + furniture (shared with the director via
    // NpcContext; navigator is also the StreamingController's).
    uptr<world::TerrainNavigator> navigator;
    gameplay::FurnitureOccupancy furnitureOccupancy;

    // B4 (chantier 1): physics — height-field tiles follow the camera (the
    // player takes over as focus in B5); the debug capsule proves the
    // fall/rest/slope behavior in-scene (drawn as the placeholder box).
    uptr<phys::PhysicsWorld> physics;
    uptr<TerrainCollision> terrainCollision;
    // Trunks + rocks from the deterministic scatter (dev report 2026-07-07).
    uptr<VegetationCollision> vegCollision;
    uptr<phys::CharacterBody> debugCapsule;
    // Chantier 2 B2 (extracted, audit U4-10): the cell-streaming fixups —
    // ground snap, static-collider cook, nav obstacles — live in
    // StreamingController behind a StreamingContext the scene builds each
    // frame. NPC (re)building stays here (NpcDirector territory); the scene
    // interleaves refreshNpcs between snap and nav to preserve order.
    StreamingController streaming;
    StreamingContext makeStreamingContext();

    // Stutter hunt: per-block frame breakdown, logged on spikes > 25 ms.
    core::FrameProbe frameProbe;

    // B5: first-person Play mode (the game IS first-person — acted
    // decision), extracted to PlayerController (audit U4-1): it owns the
    // kinematic capsule + movement/attack state, wired per call through
    // makePlayerContext(). MODE transitions stay here (SceneMode plumbing);
    // they and travel/tp drive the body via spawnBody/destroyBody, and the
    // focus/context sites read playerController.body().
    PlayerController playerController;
    PlayerContext makePlayerContext();
    // C3: refreshed each frame at the equipMods site; gates jump/sprint
    // (through the context) and feeds the equip modifiers.
    gameplay::EncumbranceCategory playerEncumbrance {
        gameplay::EncumbranceCategory::Light };
    f32 playerCarriedWeight { 0.0f };
    void enterPlayMode();
    void exitPlayMode();
    void restoreMode(SceneMode target); // drive into a mode (Escape → last mode)

    // B5.5: the player is a GAS actor (docs/STATS.md) — spawned from the
    // "Player" ActorForm, ticked by tickCharacter; the controller READS
    // the derived movementSpeed/acceleration currents and pays sprint
    // through the SprintCost GameplayEffect (§2.9: never set directly).
    gameplay::DerivedStatRegistry derivedStats;
    gameplay::GameplayTagRegistry gameTags;
    gameplay::StatsTuningForm statsTuning;
    ecs::Entity playerEntity {};
    const gameplay::EffectForm* sprintCostEffect { nullptr };
    const gameplay::EffectForm* testWoundEffect { nullptr };

    rhi::TextureHandle offscreenColor {};
    rhi::TextureHandle offscreenDepth {};
    rhi::FramebufferHandle offscreenFb {};
    // Pre-water snapshots of the opaque scene (copyTexture targets).
    rhi::TextureHandle sceneColorCopy {};
    rhi::TextureHandle sceneDepthCopy {};
    rhi::BindGroupHandle waterSceneBindGroup {};
    // Half-res mirrored scene for the water's planar reflection.
    rhi::TextureHandle reflectionColor {};
    rhi::TextureHandle reflectionDepth {};
    rhi::FramebufferHandle reflectionFb {};
    rhi::BufferHandle reflectionUbo {};
    rhi::BindGroupHandle reflectionBindGroup {};
    rhi::SamplerHandle depthSampler {}; // nearest — depth must not filter
    rhi::SamplerHandle blitSampler {};
    // B4: one blit group per adaptation ping-pong side (binding 5 = the
    // exposure texture the tonemap taps); [0] doubles as the only group
    // on the no-postFx fallback path.
    array<rhi::BindGroupHandle, 2> blitBindGroups {};
    rhi::PipelineHandle blitPipeline {};
    u64 blitShaderGeneration { 0 };
    u32 offscreenWidth { 0 };
    u32 offscreenHeight { 0 };
};

} // namespace game
