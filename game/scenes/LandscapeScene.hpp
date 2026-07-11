#pragma once

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
#include "game/MeshCache.hpp"
#include "game/scenes/GameHud.hpp"
#include "game/scenes/InteractionController.hpp"
#include "game/scenes/SceneEditor.hpp"
#include "game/scenes/StreamingController.hpp"
#include "game/scenes/UiRouter.hpp"
#include "game/scenes/NpcDirector.hpp"
#include "game/scenes/PlayerController.hpp"
#include "game/scenes/QuestDirector.hpp"
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
#include "game/TextureCache.hpp"
#include "game/VegetationCollision.hpp"
#include "game/scenes/LandscapeTuning.hpp"
#include "game/scenes/LandscapeRenderer.hpp"
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
namespace gameplay {
struct AbilityForm;  // the shared melee attack (P0 A3) — pointer only
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
    // Themed panel sections (drawUi wraps them in collapsing headers; the
    // terrain/render sections live on the renderer with their state, U4-2c).
    void drawGameplayUi();
    void drawSkyUi();

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
    bool uiPerfOpen { false }; // GPU-PERF P0: the budget table [F6]

    // U4-2c: the whole custom renderer — shader library, render::* systems,
    // GPU handles, frame graph, terrain/render dev panels and their toggle
    // state — lives in LandscapeRenderer. The scene reaches the terrain
    // ground truth via renderer.terrainParams() and hands a RenderView +
    // the RenderSnapshot to renderer.render() each frame.
    LandscapeRenderer renderer;
    bool animateTime { false };
    // Atmospheric render state (sky/fog/weather-driven), grouped so the weather
    // transition can own it (brick 3a). Manual sliders and the crossfade both
    // write here; the renderer reads it through the view. stormFront/
    // rainIntensity live here too.
    AtmosphereParams atmos;
    f32 windTime { 0.0f }; // accumulated wind phase (dt x strength)

    // B1 (chantier 1): the real mesh path replacing the H8 hardcoded cube.
    // A small ECS world spawned from plugin ReferenceForms; extractMeshes
    // fills the snapshot each frame; the residency caches resolve guids to
    // GPU resources (placeholders while pending — never block, §7).
    data::FormDatabase forms;      // resolved plugin stack (material fields
                                   //   fold into the snapshot at extract)
    data::TextTable texts;         // U4-11: LocStringForm key -> text index
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

    // Chantier 4 B3/B5/B6: the UI ACTION routing (data-event dispatch,
    // menu actions, item screens' opening, equip/use/transfer/barter) +
    // its shared state (open container/vendor, barter mults) live in
    // UiRouter (audit U4-1), wired per dispatch through
    // makeUiRouterContext(). GameHud reads the pricing state through the
    // router's accessors; dialogue OPENING stays here (quest territory).
    UiRouter uiRouter;
    UiRouterContext makeUiRouterContext();

    // Chantier 6 A2 / D2 + chantier 4 B4 — quests, crime and dialogue
    // extracted behind QuestDirector (audit U4-1): the demo quest state
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

    // Chantier 4 B5: barter data (gold is an ordinary item; the routing
    // and the vendor multipliers live in UiRouter).
    core::Rng lootRng { 0x4d7a9b30u }; // loadout rolls (§8 seeded)
    core::Rng combatRng { 0x50A5B10Cu }; // NPC combat rolls — A5 guards (§8)
    // P0 B1: the frame's actor snapshot grid — rebuilt once per sim tick,
    // read by the trigger sweep and (B2/B3) perception / combat AI.
    world::SpatialIndex spatialIndex;
    const data::MiscItemForm* goldForm { nullptr };

    // Chantier 5 B3: the one post-spawn seam for EVERY actor (player and
    // NPC): stat init, then saved state (when this actor was captured —
    // its SavedStatsForm is the sentinel) or the data loadout. Returns
    // true when saved state applied (fresh-game extras skip then).
    bool finalizeActorSpawn(ecs::Entity entity,
                            const core::Guid& actorFormId);

    // Chantier 5 B4/B5 — disk saves + the pending in-memory layer (the
    // memory of unloaded cells: looted crates stay looted without a disk
    // save). Extracted behind SaveController (audit U4-1): it owns the
    // pending layer (hooked into CellLoader each onEnter via pending()),
    // the queued-reload flags and the capture/flush serialization. The
    // load-APPLICATION half (WorldStateForm → clock/worldspace/camera)
    // stays here, woven into onEnter.
    SaveController saveController;
    SaveContext makeSaveContext();
    std::optional<gameplay::WorldStateForm> loadedWorldState;

    // Chantier 4 B7: dev console in the game scene (F8). The panel / VM /
    // session infrastructure + visibility + god mode live in SceneConsole
    // (audit U4-1); createConsole registers the WORLD commands (spawn/tp/
    // tgm/save/settime) onto its panel — they touch scene internals so they
    // stay here (the event-subscription rationale).
    SceneConsole sceneConsole;
    void createConsole();

    // Chantier 3 B5/B6: melee combat — everything flows through the GAS
    // damage pipeline (weaponDamageEvent -> applyDamage), like the 2D
    // CombatArena. P0 A3: swings are MeleeSwing state machines gated by
    // the shared attack ability; damage lands where the blade passes.
    const data::WeaponForm* playerWeapon { nullptr };
    const data::WeaponForm* banditWeapon { nullptr };
    const gameplay::AbilityForm* attackAbility { nullptr };
    const gameplay::AbilityForm* dodgeAbility { nullptr };

    // Chantier 2 B8: the authored-terrain overlay. IMMUTABLE once
    // published; the sculpt tool edits a working copy then publishes a
    // NEW instance. Lifetime is carried by TerrainParams.patches itself
    // (shared_ptr — worker-held copies keep old instances alive, even
    // across scene teardown).
    sptr<const render::HeightPatches> heightPatches;
    uptr<TextureCache> materialTextures; // SRGBA8 + Linear (3D albedo)
    uptr<MeshCache> meshCache;
    RenderSnapshot snapshot;

    // B6 (chantier 1): Forms-driven skinned NPCs — the sim subsystem
    // (rig cache, NPC list, build/AI/schedule/combat) lives in NpcDirector
    // (audit U4-10), behind an NpcContext the scene builds each call. The
    // scene keeps cross-cutting reads via npcDirector.npcs() (player
    // attack/crime, debug UI, editor, console). U4-2b: the DRAW side runs
    // from snapshot.skinned, inside the renderer.
    NpcDirector npcDirector;
    NpcContext makeNpcContext();
    // Thin delegators kept so the many call sites stay unchanged; each just
    // bundles the context and forwards to the director.
    void refreshNpcs(rhi::Device& device);
    void updateNpcs(f32 dt);

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
};

} // namespace game
