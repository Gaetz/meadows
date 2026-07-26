#pragma once

// Subsystem map: docs/AUDIT/U4-landscapescene.md

#include <optional>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/LocForms.hpp"
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
#include "engine/render/MeshCache.hpp"
#include "game/InputActions.hpp"
#include "game/Settings.hpp"
#include "game/scenes/GameHud.hpp"
#include "game/scenes/InteractionController.hpp"
#include "game/scenes/MapController.hpp"
#include "game/scenes/OptionsController.hpp"
#include "game/scenes/SceneEditor.hpp"
#include "game/scenes/StreamingController.hpp"
#include "game/scenes/UiRouter.hpp"
#include "game/scenes/FollowerController.hpp"
#include "game/scenes/NpcDirector.hpp"
#include "game/scenes/PlayerController.hpp"
#include "game/scenes/QuestDirector.hpp"
#include "game/scenes/RideController.hpp"
#include "game/scenes/SaveController.hpp"
#include "game/scenes/SceneConsole.hpp"
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
#include "engine/fx/Particles.hpp"
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
#include "world/scene/SpatialIndex.hpp"
#include "world/streaming/CellStreamer.hpp"
#include "game/SceneSubmit.hpp"
#include "game/TerrainCollision.hpp"
#include "engine/render/TextureCache.hpp"
#include "game/VegetationCollision.hpp"
#include "game/scenes/LandscapeTuning.hpp"
#include "engine/audio/Audio.hpp"
#include "game/SoundResolver.hpp"
#include "game/scenes/FxDirector.hpp"
#include "game/scenes/LandscapeRenderer.hpp"
#include "game/scenes/ProjectileDirector.hpp"
#include "engine/rhi/Rhi.hpp"
#include "game/Scene.hpp"

namespace engine {
class Engine;
}
namespace data {
struct WeaponForm;   // CoreForms — pointers only in this header
struct MiscItemForm; // gold
class EditSession;   // console
}
namespace gameplay {
struct AbilityForm;  // the shared melee attack — pointer only
}
namespace script {
class Vm;
}

namespace game {

class ConsolePanel;

// The 3D landscape renderer prototype (the custom-renderer path,
// docs/RENDERING.md).
// Owns the frame: records its own render passes instead of the sprite path.
// Bloom (soft-threshold HDR pyramid, additive upsample) and
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
    // Themed panel sections (drawUi wraps them in collapsing headers; the
    // terrain/render sections live on the renderer with their state).
    void drawGameplayUi();
    void drawSkyUi();
    // The render panels' Save button: current live values -> FULL patch
    // records on the canonical tuning records, written as the
    // mods/render-tuning.toml overlay plugin (§5 — one more layer, base
    // data untouched; full records so a re-save never drops a field).
    void saveRenderTuning();

    engine::Engine* engine { nullptr };

    // Moddable startup values (§5): loaded from data/base/landscape.toml
    // (plus any mod patches) in onEnter, then copied into the systems' plain
    // params and the UI members below — the panel still adjusts everything
    // live; the TOML sets where it all starts.
    data::FormTypeRegistry formTypes;
    LandscapeTuningForm tuning;

    // Weather (extracted to WeatherController): precreated
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
    bool uiTreesOpen { false }; // Tree builder (generation knobs, live)
    bool uiPerfOpen { false }; // the GPU budget table [F6]

    // The whole custom renderer — shader library, render::* systems,
    // GPU handles, frame graph, terrain/render dev panels and their toggle
    // state — lives in LandscapeRenderer. The scene reaches the terrain
    // ground truth via renderer.terrainParams() and hands a RenderView +
    // the RenderSnapshot to renderer.render() each frame.
    LandscapeRenderer renderer;
    bool animateTime { false };
    // Atmospheric render state (sky/fog/weather-driven), grouped so the weather
    // transition can own it. Manual sliders and the crossfade both
    // write here; the renderer reads it through the view. stormFront/
    // rainIntensity live here too.
    AtmosphereParams atmos;
    f32 windTime { 0.0f }; // accumulated wind phase (dt x strength)

    // The real mesh path.
    // A small ECS world spawned from plugin ReferenceForms; extractMeshes
    // fills the snapshot each frame; the residency caches resolve guids to
    // GPU resources (placeholders while pending — never block, §7).
    data::FormDatabase forms;      // resolved plugin stack (material fields
                                   //   fold into the snapshot at extract)
    data::TextTable texts;         // LocStringForm key -> text index
    // Machine preferences + the action layer — loaded from
    // settings.toml at enter, written back by the options screen.
    game::Settings settings;
    game::ActionMap actionMap;
    data::PluginStack pluginStack; // owns the plugins behind `forms`
    assets::AssetDatabase assetDb; // guid -> file, layered per plugin order
    ecs::World world;
    // Cached flecs queries for the PER-FRAME paths (creating a query is an
    // allocation + registry insert — never per frame). Handles into
    // `world`; rebuilt right after it in onEnter.
    flecs::query<const world::Transform, const world::DoorTarget> doorQuery;
    flecs::query<const world::Transform, const world::RefId> interactQuery;

    // Cells stream around the player (synchronous ring —
    // async streaming may come later). References
    // with no cell are persistent (the player), spawned once at enter.
    world::FormCategoryRegistry categories; // must outlive the CellLoader
    world::Spawner spawner;
    world::WorldModel worldModel;
    uptr<world::CellLoader> cellLoader;
    uptr<world::CellStreamer> cellStreamer;
    data::FormHandle overworldHandle {};

    // Worldspace travel through doors. `activeWorldspace`
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

    // Level editor + terrain sculpt, extracted to
    // SceneEditor. The scene owns the EditSession (levelEditor)
    // and the systems; SceneEditor owns the editor STATE (selection, palette,
    // gizmo, sculpt tool) and the interaction/UI. Wired each frame through
    // EditorContext (makeEditorContext, which folds in the sculpt sub-contract
    // makeSculptContext). Target: SceneEditor becomes a stacked SceneStack layer.
    uptr<LevelEditor> levelEditor;
    SceneEditor sceneEditor;
    EditorContext makeEditorContext();
    SculptContext makeSculptContext();
    // GENERIC interaction (E) + travel fade + talk toast,
    // extracted to InteractionController. performTravel STAYS
    // here (a worldspace swap is streaming/scene territory — cellStreamer,
    // colliders, NPC refresh, player capsule); the controller fires it
    // through the InteractionContext travel callback at the black of the
    // fade. Wired each call through makeInteractionContext().
    InteractionController interaction;
    InteractionContext makeInteractionContext();
    void performTravel(const core::Guid& targetReference);

    // The game clock owns time-of-day (the sky follows)
    // and feeds real game-time into tickCharacter/schedules.
    gameplay::GameClock gameClock;

    // The RmlUi game UI. Screens come from UiScreenForm
    // records (documents through the plugins' ui/ roots — the SkyUI
    // model); the ScreenStack decides what is visible, a modal screen
    // pauses the sim and owns mouse/keyboard. Dev panels stay ImGui.
    ::ui::UiSystem uiSystem; // ::ui — game::ui (panels) masks it
    ScreenStack screenStack;
    bool uiCreated { false };
    bool uiModalWasOpen { false };
    bool uiTextInputOn { false };
    // Pad-driven UI navigation state — the left-stick repeat
    // cooldown, and the A/B edges the UI consumed. Those buttons stay
    // "owned by the UI" until physically released, so closing a menu
    // with B cannot tap-dodge, nor activating with A jump (A = Jump,
    // B = SprintDodge in the default pad bindings).
    f32 uiStickCooldown { 0.0f };
    bool uiPadConsumedA { false };
    bool uiPadConsumedB { false };
    vector<str> shownScreens; // documents currently shown (sync state)
    // onEnter phases: onEnter() runs these in order. Split for
    // readability only.
    void bootstrapData();                             // plugins/save/tuning
    void createRenderResources(rhi::Device& device);  // GPU resources + game UI
    void setupGameplay();                             // physics/nav/stats/quest
    void setupWorldAndStreaming();                    // ECS world, cells, clock
    void spawnInitialWorld(rhi::Device& device);      // spawn + npc + camera

    void createGameUi(rhi::Device& device);
    void updateGameUi(f32 dt);
    void syncScreens();
    vector<const ScreenStack::Screen*> screenStackPreloadList() const;

    // Every push*Model / update*Model (game state -> UiSystem
    // data models) plus the view-model state (InventoryViews, dialogue
    // options) lives in GameHud, wired per call through makeHudContext().
    // Game ACTIONS stay here (handleUiEvent/handleMenuAction, equip/use/
    // transfer/barter, open*Screen) and mutate the views via hud accessors.
    GameHud hud;
    HudContext makeHudContext();

    // The UI ACTION routing (data-event dispatch,
    // menu actions, item screens' opening, equip/use/transfer/barter) +
    // its shared state (open container/vendor, barter mults) live in
    // UiRouter, wired per dispatch through
    // makeUiRouterContext(). GameHud reads the pricing state through the
    // router's accessors; dialogue OPENING stays here (quest territory).
    UiRouter uiRouter;
    UiRouterContext makeUiRouterContext();

    // The options screen (look/volume steppers, bindings table,
    // press-to-rebind capture). While a capture is armed it owns the
    // whole input frame — updateGameUi gates Tab/Pause and the
    // pad->UI routing on optionsController.capturing().
    OptionsController optionsController;
    OptionsContext makeOptionsContext();
    // The in-game map — CPU raster of the exterior worldspace
    // (game/MapRaster) behind the runtime://map texture, player marker +
    // door POIs through the shared mapUv mapping.
    MapController mapController;
    MapContext makeMapContext();
    // The language machinery. loadGatedPluginConfig = plugins.toml
    // with every text-<code>.toml pack enabled iff <code> ==
    // settings.language; applyLanguage = the options screen's LIVE switch
    // (TextTable rebuild from a temp resolve + relocalize — no resolved
    // Form pointer moves; `forms` re-resolves gated on the next enter).
    data::PluginConfig loadGatedPluginConfig(
        const std::filesystem::path& dataDir) const;
    void applyLanguage();

    // Quests, crime and dialogue
    // extracted behind QuestDirector: the demo quest state
    // machine, its mirror (and the crime bounty's) into PLAYER tags so
    // dialogue options gate on them through the condition evaluator, and
    // the dialogue runner. The eventBus stays a SCENE hub (dialogue and
    // combat both publish into it); the subscriptions live in onEnter and
    // delegate to the director. makeEvalContext stays here (generic player
    // condition context, also feeds the HUD).
    QuestDirector questDirector;
    QuestContext makeQuestContext();
    gameplay::EventBus eventBus;
    gameplay::EvalContext makeEvalContext() const;

    // Barter data (gold is an ordinary item; the routing
    // and the vendor multipliers live in UiRouter).
    core::Rng lootRng { 0x4d7a9b30u }; // loadout rolls (§8 seeded)
    core::Rng combatRng { 0x50A5B10Cu }; // NPC combat rolls (§8 seeded)
    // The frame's actor snapshot grid — rebuilt once per sim tick,
    // read by the trigger sweep and perception / combat AI.
    world::SpatialIndex spatialIndex;
    // The CPU particle sim (headless engine/fx) — updated with
    // the sim tick, extracted as POD batches, drawn by the FxRenderer.
    fx::ParticleSim fxSim;
    // The standard cue handlers (CueForm -> particles/shake) —
    // combat emits into fxDirector.cues(), presentation follows.
    FxDirector fxDirector;
    // The audio seam — the real backend in-game (null when headless), the
    // resolver maps SoundForms onto it; cue sounds ride the FxDirector.
    audio::AudioSystem audioSystem;
    SoundResolver soundResolver;
    // Arrows in flight (player bow, archer NPCs).
    ProjectileDirector projectileDirector;
    const data::MiscItemForm* goldForm { nullptr };

    // The one post-spawn seam for EVERY actor (player and
    // NPC): stat init, then saved state (when this actor was captured —
    // its SavedStatsForm is the sentinel) or the data loadout. Returns
    // true when saved state applied (fresh-game extras skip then).
    bool finalizeActorSpawn(ecs::Entity entity,
                            const core::Guid& actorFormId);

    // Disk saves + the pending in-memory layer (the
    // memory of unloaded cells: looted crates stay looted without a disk
    // save). Extracted behind SaveController: it owns the
    // pending layer (hooked into CellLoader each onEnter via pending()),
    // the queued-reload flags and the capture/flush serialization. The
    // load-APPLICATION half (WorldStateForm → clock/worldspace/camera)
    // stays here, woven into onEnter.
    SaveController saveController;
    SaveContext makeSaveContext();
    std::optional<gameplay::WorldStateForm> loadedWorldState;

    // Dev console in the game scene (F8). The panel / VM /
    // session infrastructure + visibility + god mode live in SceneConsole
    //; createConsole registers the WORLD commands (spawn/tp/
    // tgm/save/settime) onto its panel — they touch scene internals so they
    // stay here (the event-subscription rationale).
    SceneConsole sceneConsole;
    void createConsole();

    // Melee combat — everything flows through the GAS
    // damage pipeline (weaponDamageEvent -> applyDamage), like the 2D
    // CombatArena. Swings are MeleeSwing state machines gated by
    // the shared attack ability; damage lands where the blade passes.
    const data::WeaponForm* playerWeapon { nullptr };
    const data::WeaponForm* banditWeapon { nullptr };
    const gameplay::AbilityForm* attackAbility { nullptr };
    const gameplay::AbilityForm* dodgeAbility { nullptr };
    const gameplay::EffectForm* swimCostEffect { nullptr }; // D2b
    const gameplay::EffectForm* sneakCostEffect { nullptr }; // sneak
    const gameplay::EffectForm* bowDrawCostEffect { nullptr }; // drawn-bow drain

    // The authored-terrain overlay. IMMUTABLE once
    // published; the sculpt tool edits a working copy then publishes a
    // NEW instance. Lifetime is carried by TerrainParams.patches itself
    // (shared_ptr — worker-held copies keep old instances alive, even
    // across scene teardown).
    sptr<const render::HeightPatches> heightPatches;
    uptr<render::TextureCache> materialTextures; // SRGBA8 + Linear (3D albedo)
    uptr<render::MeshCache> meshCache;
    RenderSnapshot snapshot;

    // Forms-driven skinned NPCs — the sim subsystem
    // (rig cache, NPC list, build/AI/schedule/combat) lives in NpcDirector,
    // behind an NpcContext the scene builds each call. The
    // scene keeps cross-cutting reads via npcDirector.npcs() (player
    // attack/crime, debug UI, editor, console). The DRAW side runs
    // from snapshot.skinned, inside the renderer.
    NpcDirector npcDirector;
    NpcContext makeNpcContext();
    // Thin delegators kept so the many call sites stay unchanged; each just
    // bundles the context and forwards to the director.
    void refreshNpcs(rhi::Device& device);
    void updateNpcs(f32 dt);

    // Recruit/dismiss (the cell->0 persistence
    // contract through the pending layer) + the party teleports, behind
    // the usual *Controller pattern; wired per call through
    // makeFollowerContext(). The dialogue events OnRecruitFollower /
    // OnDismissFollower subscribe in onEnter (the OpenBarter precedent).
    FollowerController followerController;
    FollowerContext makeFollowerContext();

    // Navigation + furniture (shared with the director via
    // NpcContext; navigator is also the StreamingController's).
    uptr<world::TerrainNavigator> navigator;
    gameplay::FurnitureOccupancy furnitureOccupancy;

    // Physics — height-field tiles follow the camera (the
    // player takes over as focus in Play); the debug capsule proves the
    // fall/rest/slope behavior in-scene (drawn as the placeholder box).
    uptr<phys::PhysicsWorld> physics;
    uptr<TerrainCollision> terrainCollision;
    // Trunks + rocks from the deterministic scatter.
    uptr<VegetationCollision> vegCollision;
    uptr<phys::CharacterBody> debugCapsule;
    // (extracted): the cell-streaming fixups —
    // ground snap, static-collider cook, nav obstacles — live in
    // StreamingController behind a StreamingContext the scene builds each
    // frame. NPC (re)building stays here (NpcDirector territory); the scene
    // interleaves refreshNpcs between snap and nav to preserve order.
    StreamingController streaming;
    StreamingContext makeStreamingContext();

    // Stutter hunt: per-block frame breakdown, logged on spikes > 25 ms.
    core::FrameProbe frameProbe;

    // The `torchbench` console command's transient light entities
    // (docs/RENDERING.md §5 B0) — cleared on re-run and on exit.
    vector<ecs::Entity> benchLights;

    // First-person Play mode (the game IS first-person — acted
    // decision), extracted to PlayerController: it owns the
    // kinematic capsule + movement/attack state, wired per call through
    // makePlayerContext(). MODE transitions stay here (SceneMode plumbing);
    // they and travel/tp drive the body via spawnBody/destroyBody, and the
    // focus/context sites read playerController.body().
    PlayerController playerController;
    PlayerContext makePlayerContext();
    // Riding v1 (tech proof): while mounted the capsule is
    // destroyed and RideController runs INSTEAD of PlayerController —
    // one if/else at the update call site, PlayerController untouched.
    // Mount/dismount arrive through the interaction closure; travel and
    // mode exits force a dismount first.
    RideController rideController;
    RideContext makeRideContext();
    // C3: refreshed each frame at the equipMods site; gates jump/sprint
    // (through the context) and feeds the equip modifiers.
    gameplay::EncumbranceCategory playerEncumbrance {
        gameplay::EncumbranceCategory::Light };
    f32 playerCarriedWeight { 0.0f };
    void enterPlayMode();
    void exitPlayMode();
    void restoreMode(SceneMode target); // drive into a mode (Escape → last mode)

    // The player is a GAS actor (docs/STATS.md) — spawned from the
    // "Player" ActorForm, ticked by tickCharacter; the controller READS
    // the derived movementSpeed/acceleration currents and pays sprint
    // through the SprintCost GameplayEffect (§2.9: never set directly).
    gameplay::DerivedStatRegistry derivedStats;
    gameplay::GameplayTagRegistry gameTags;
    gameplay::StatsTuningForm statsTuning;
    ecs::Entity playerEntity {};
    const gameplay::EffectForm* sprintCostEffect { nullptr };
    const gameplay::EffectForm* testWoundEffect { nullptr };
};

} // namespace game
