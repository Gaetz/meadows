#pragma once

// Subsystem map: docs/AUDIT/U4-landscapescene.md

#include <deque>
#include <unordered_set>
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
#include "engine/terrain/WaterQuery.hpp"
#include "game/LevelEditor.hpp"
#include "game/TerrainBakeStreamer.hpp"
#include "engine/render/MeshCache.hpp"
#include "game/InputActions.hpp"
#include "game/Settings.hpp"
#include "game/scenes/GameHud.hpp"
#include "game/scenes/InteractionController.hpp"
#include "game/scenes/MapController.hpp"
#include "game/scenes/SpiritDirector.hpp"
#include "world/spirit/Spells.hpp"
#include "world/spirit/WaterReading.hpp"
#include "world/terrain/TerrainBrush.hpp"
#include "game/scenes/MiniMapPanel.hpp"
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
#include "engine/render/AtmosphereParams.hpp"
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
#include "world/ai/InteriorNavigator.hpp"
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
#include "engine/render/WorldRenderer.hpp"
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
struct ScriptContext;
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
    // `bootLoadSlot`: a save slot queued for the FIRST enter (the
    // tree-creator round trip boots from its hidden save; the file is
    // deleted once consumed). Empty = ordinary fresh boot.
    explicit LandscapeScene(engine::Engine& engineContext,
                            str bootSlot = {})
        : engine(&engineContext), bootLoadSlot(std::move(bootSlot)) {}

    // The tree-creator round trip's hidden save slot.
    static constexpr const char* kTreeCreatorSlot = "treecreator";

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
    bool uiMapOpen { false };  // dev minimap (MiniMapPanel)

    // The whole custom renderer — shader library, render::* systems,
    // GPU handles, frame graph, terrain/render dev panels and their toggle
    // state — lives in render::WorldRenderer. The scene reaches the terrain
    // ground truth via renderer.terrainParams() and hands a render::RenderView +
    // the RenderSnapshot to renderer.render() each frame.
    render::WorldRenderer renderer;
    bool animateTime { false };
    // Atmospheric render state (sky/fog/weather-driven), grouped so the weather
    // transition can own it. Manual sliders and the crossfade both
    // write here; the renderer reads it through the view. stormFront/
    // rainIntensity live here too.
    render::AtmosphereParams atmos;
    f32 windTime { 0.0f }; // accumulated wind phase (dt x strength)
    // The drift the clouds, the sky volume and the mist ride: the wind's
    // direction integrated with the same weight — a turning wind curves
    // their path instead of teleporting the pattern (direction x clock did).
    Vec2 windDrift { 0.0f };

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
    // Props carrying flames (world::FxSource — torches, campfires): one
    // emitter set per entity while it lives within reach of the camera.
    flecs::query<const world::Transform, world::FxSource> fxSourceQuery;
    struct FxSourceEmitters {
        u32 flame { 0 };
        u32 smoke { 0 };
        u32 haze { 0 };
        audio::AudioSystem::SoundId sound { 0 };
    };
    std::unordered_map<u64, FxSourceEmitters> fxSourceEmitters;
    // The frame's water (douses, jet lumps, splashes, springs) and fire
    // (sparks, brands, the jet's pulses) events, as discs on the ground:
    // what snuffs a torch and what relights it. Cleared once consumed.
    struct GroundEvent {
        Vec2 at { 0.0f };
        f32 radius { 0.0f };
    };
    vector<GroundEvent> waterEvents;
    vector<GroundEvent> fireEvents;
    // A spell's spark on the ground: the lane's ignition, remembered for
    // the frame (the props that relight from it).
    void igniteGround(f32 x, f32 z, f32 radius, f32 heat);
    void snuffFxSource(ecs::Entity entity, world::FxSource& source, const Vec3& at);
    static constexpr f32 kFxSourceReach = 90.0f;
    void updateFxSources();
    void resetFxSources(); // map swap / exit: the emitters go with the world
    // The igniters among them (FxSource.igniteHeat > 0), rebuilt each
    // frame: their heat goes to the wooden props and trees within reach
    // (per frame, waking the fire lane so its job ticks them) and to the
    // ground when the flame stands within a metre of it.
    struct FxIgniter {
        Vec3 at { 0.0f };
        f32 radius { 0.0f };
        f32 heat { 0.0f };
        bool ground { false };
    };
    vector<FxIgniter> fxIgniters;
    f32 fxIgniterTreeClock { 0.0f };
    bool fxIgniterNearTree { false };
    void applyFxIgniters(f32 dt);
    bool nearFxIgniter(const Vec3& at) const;
    // A wooden prop catches: its flames, sparks and smoke, the record
    // shared by the fire's own heat (updateSpiritFireProps) and an igniter's.
    void lightProp(ecs::Entity entity, const Vec3& position, f32 fuel);

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
    // Last travel's arrival marker: in an interior this is the way OUT —
    // where broken fighters flee (NpcContext.interiorExit).
    Vec3 interiorArrival { 0.0f };
    // Fighters who fled through the interior's exit: despawned inside
    // (their reference disabled through the pending layer), respawned
    // outside the door if the player follows within 20 s — any longer
    // and they made a clean getaway.
    struct InteriorEscape {
        core::Guid baseForm;
        f32 at;     // timeSeconds stamp
        f32 health; // base health at the door — his wounds follow him out
    };
    vector<InteriorEscape> interiorEscapes;

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
    DungeonGenContext makeDungeonGenContext();
    // The resolved map-scale bake params — ONE definition shared by
    // applyMapWorld's streamer and the editor's map bake (M5.3).
    render::terraingen::TileBakeParams makeMapBakeParams() const;
    // GENERIC interaction (E) + travel fade + talk toast,
    // extracted to InteractionController. performTravel STAYS
    // here (a worldspace swap is streaming/scene territory — cellStreamer,
    // colliders, NPC refresh, player capsule); the controller fires it
    // through the InteractionContext travel callback at the black of the
    // fade. Wired each call through makeInteractionContext().
    InteractionController interaction;
    InteractionContext makeInteractionContext();
    // Equipment + encumbrance modifiers for the player's character tick
    // (also refreshes playerCarriedWeight/playerEncumbrance for the HUD).
    gameplay::StatModifiers playerEquipMods();
    // Sleep in a bed / wait at a campfire: the gameplay time-skips over
    // the player (the InteractionContext applySleep/applyWait closures).
    void applySleep(f32 hours);
    void applyWait(f32 hours);
    bool statsPanelOpen { false }; // F7: player character-stats inspector
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
    MiniMapPanel miniMap; // Spectator/Edit dev minimap
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
    // The spirit ability behind Q (chantier ESPRITS): its Lua script
    // acts through the world actions bound on the Vm. Selected by the
    // console (`spirit cast <EditorId>`) until a spirit bar exists.
    const gameplay::AbilityForm* spiritAbility { nullptr };
    const gameplay::EffectForm* swimCostEffect { nullptr }; // D2b
    const gameplay::EffectForm* sneakCostEffect { nullptr }; // sneak
    const gameplay::EffectForm* bowDrawCostEffect { nullptr }; // drawn-bow drain

    // The authored-terrain overlay. IMMUTABLE once
    // published; the sculpt tool edits a working copy then publishes a
    // NEW instance. Lifetime is carried by TerrainParams.patches itself
    // (shared_ptr — worker-held copies keep old instances alive, even
    // across scene teardown).
    sptr<const render::HeightPatches> heightPatches;
    // The baked-base layer under it (generated terrain regions), same
    // immutable-publish contract.
    sptr<const render::TerrainBase> terrainBase;
    // Sandbox mode: the tile streamer and the water bodies its bakes
    // emitted (rendered/queried by the water systems).
    uptr<TerrainBakeStreamer> bakeStreamer;
    bool sandboxActive { false };
    // The probed sandbox start (stable per seed): survives the story
    // camera-init that runs later in load().
    Vec3 sandboxSpawn { 0.0f };
    bool sandboxSpawnValid { false };
    // Snow altitude of the ACTIVE mode (story: tuning.snowLine, sandbox:
    // tuning.sandboxSnowLine) — feeds both params.snowLine (CPU rules)
    // and the render view (shader), so they stay in lockstep.
    f32 activeSnowLine { render::kSnowLine };
    vector<render::terraingen::Lake> sandboxLakes;
    vector<render::terraingen::River> sandboxRivers;
    // Local water bodies (sea + lakes + rivers): Forms + sandbox bakes,
    // immutable-publish; swim queries and WaterSystem share it.
    sptr<const render::WaterBodies> waterBodies;
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
    // Multi-level grid for the CURRENT interior worldspace (dungeons);
    // rebuilt on travel from its NavGridForm, null outdoors.
    uptr<world::InteriorNavigator> interiorNavigator;
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
    // Sandbox: lands the frame's finished tiles as ONE transaction —
    // a single TerrainBase publish, one water rebuild, one collision
    // rebuild and one queue pass however many tiles arrived (N per-tile
    // publishes multiplied every cost during warmups).
    void publishBakedTiles(
        vector<TerrainBakeStreamer::PublishedTile>&& tiles,
        const Vec3& focus);
    // Rebuilds waterBodies from Forms + sandbox results and hands it to
    // the swim queries and the WaterSystem.
    void publishWaterBodies();
    // Main-menu game mode: story (authored world, legacy terrain) or
    // sandbox (infinite generated world + streamer). Idempotent; sandbox
    // also moves the fly camera to a pleasant generated start so the
    // play capsule spawns there.
    void setSandboxMode(bool enable);
    // The ONE map-swap transaction (chantier CARTES M2.3): terrain
    // identity, streamer, water closures, transient clears, collision
    // rebuild, content invalidation — mode switch and map travel share
    // it.
    void applyMapWorld(i32 mapX, i32 mapZ);
    // Console-driven map travel (M4.1); the pass/door path (M4.2)
    // reuses it. `arrival` (nullable) lands the traveler THERE instead
    // of the map's probed spawn — the far side of a pass.
    void travelToMap(i32 mapX, i32 mapZ, const Vec2* arrival = nullptr);
    Vec3 probeSandboxSpawn() const;
    // The active bounded map (set by applyMapWorld; saved/restored).
    i32 activeMapX { 0 };
    i32 activeMapZ { 0 };
    // A map crossing asked mid-frame (pass trigger Lua): executed at a
    // safe point, never inside an ECS iteration.
    struct PendingMapTravel {
        i32 mapX { 0 };
        i32 mapZ { 0 };
        bool hasArrival { false };
        Vec2 arrival { 0.0f, 0.0f };
    };
    std::optional<PendingMapTravel> pendingMapTravel;
    // Chantier ESPRITS: the placed spirit sources (springs...). Their
    // lifetimes run in SIM seconds; the water kernel reads value copies.
    SpiritDirector spiritDirector;
    void pushRuntimeWaterSources();
    core::Guid activeWorldspaceGuid() const;
    // The ability -> world seam. A script's `spirit.spawn(...)` runs
    // mid-frame (a coroutine resume, a trigger dispatch) so it only
    // QUEUES here; the same safe point as pendingMapTravel applies.
    struct PendingSpiritAction {
        render::terrain::SpiritKind kind {};
        f32 x { 0.0f };
        f32 z { 0.0f };
        f32 rate { 0.0f };
        f32 radius { 0.0f };
        f32 seconds { 0.0f };
        // Source: placed at (x, z). Jet: streamed from the caster's
        // nozzle along the aim at `speed`. Hold: the control spell's
        // held volume, drawn at the aim while the key is held.
        // EarthBump: an instant mound at (x, z) that throws what stands
        // on it. EarthDig: the ground lowered under the aim while held.
        // EarthBrush: the ground raised (create) or lowered (destroy) under
        // the aim while held. EarthWall: a ridge from the press spot to the
        // release spot.
        // EarthSeize: the nearest rock within `radius` of the aim, no
        // bigger than `rate` metres, carried while held.
        // FireIgnite: `rate` heat dealt to the fire field's cells within
        // `radius` of (x, z) — the field spreads it.
        // FireDouse: the burning cells within `radius` of (x, z) put out.
        // FireBrand: a flame carried at the aim while held, igniting
        // `radius` around it with `rate` heat every costPeriod.
        // FireGlobe: the fire ward around the caster while held — nothing
        // burns within `radius`, the caster feels no flame.
        // FireStream: the flame jet — a short cone ahead of the caster while
        // held, igniting the ground along it (`rate` heat, `radius`) and
        // burning whoever stands in it, out to `range`.
        // WindBlow: the breath — a gust carried at the aim while held.
        // WindCalm / WindDirect: the weather's wind stilled / steered
        // where the caster faces, while held.
        enum class Mode : u8 { Source, Jet, Hold, EarthBump, EarthBrush,
                               EarthDig, EarthWall, EarthSeize, FireIgnite,
                               FireDouse, FireBrand, FireGlobe, FireStream,
                               WindBlow, WindCalm, WindDirect };
        Mode mode { Mode::Source };
        f32 speed { 0.0f };
        f32 range { 0.0f };
        f32 dirX { 0.0f }; // wind sources: the direction they blow toward
        f32 dirZ { 0.0f };
        bool channeled { false };
        f32 costPeriod { 1.0f };
        f32 upkeepScale { 0.25f };
    };
    std::deque<PendingSpiritAction> pendingSpiritActions;
    void applyPendingSpiritActions();
    // Jets follow the caster every frame: re-aim, re-land, steer their
    // particle stream, feed the water kernel. Play mode only.
    void updateSpiritJets(f32 dt);
    void stopSpiritEmitters(const vector<u32>& emitters);
    // The caster's nozzle (eye ray, hand-height offset).
    Vec3 spiritNozzle() const;
    // The stream is lumps launched at this cadence (seconds).
    static constexpr f32 kJetSphereInterval = 0.12f;
    // Earth: the dig in progress (world/terrain/TerrainBrush working
    // grids, previewed at ~20 Hz, committed once on release), and the
    // bump. rate = metres per second lowered (dig) / metres raised (bump),
    // divided by the ground's hardness (SurfaceMaterialForm).
    struct SpiritEarth {
        world::BrushGrids grids;
        bool raise { false }; // the stone brush raises, the dig lowers
        f32 rate { 0.0f };
        f32 radius { 4.0f };
        f32 remaining { 0.0f };
        f32 previewTimer { 0.0f };
        f32 cueTimer { 0.0f };
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        core::Guid ability;
    };
    std::optional<SpiritEarth> spiritEarth;
    void applyEarthBump(const PendingSpiritAction& action);
    void updateSpiritEarth(f32 dt);
    void finishSpiritEarth(bool commit);
    f32 groundHardnessAt(f32 x, f32 z) const;
    // Rocks the earth spirit seized: their static body became a dynamic
    // convex hull (engine/physics), carried kinematically while held,
    // free afterwards — the entity's Transform follows the body.
    struct SpiritRock {
        ecs::Entity entity;
        phys::BodyId body { 0 };
        bool held { false };
        f32 radius { 1.0f };
        Vec3 lastTarget { 0.0f };
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        core::Guid ability;
    };
    vector<SpiritRock> spiritRocks;
    // Chunks whose ground the spirits (or a Play-mode stroke) reshaped —
    // the save writes their .ter (E2.b); seeded on load from the save's
    // own TerrainPatchForm records so a re-save keeps them.
    std::unordered_set<u64> spellTouchedChunks;
    void seizeRock(const PendingSpiritAction& action);
    void updateSpiritRocks(f32 dt);
    void releaseSpiritRocks(); // map swap / exit: bodies go with the world
    // The fire lane (SpiritDirector::updateFire): the per-frame job kick,
    // the scorch mask upload when a job lands, and the flame emitters
    // re-placed on the burning cells nearest the camera under a budget
    // (the Far Cry 2 "hair transplant").
    void updateSpiritFire(f32 simSeconds);
    void resetSpiritFire(); // map swap / exit: window, mask and flames go
    struct FlameEmitter {
        Vec2 at { 0.0f }; // the cell center (exact: matched by equality)
        u32 emitter { 0 }; // the flames
        u32 sparks { 0 };  // the embers it sheds
        u32 smoke { 0 };   // one cell in four smokes (discreet)
        u32 haze { 0 };    // the heat shimmer, near cells only
    };
    vector<FlameEmitter> flameEmitters;
    static constexpr f32 kFlameHazeNear = 30.0f; // shimmer within this
    static constexpr u32 kMaxFlames = 96;
    static constexpr f32 kFlameReach = 120.0f;
    static constexpr f32 kFlameBehindRadius = 12.0f; // behind the camera, flames only this close
    // Beyond this, one flame stands for a 2x2 block of cells (bigger,
    // sparser): the far front costs a quarter of the emitters.
    static constexpr f32 kFlameLodNear = 40.0f;
    // The field's 3D loop (SpiritForm.fieldSound): one source that
    // follows the burning cell nearest the camera, silent beyond reach.
    audio::AudioSystem::SoundId fireLoop { 0 };
    static constexpr f32 kFireSoundReach = 60.0f;
    void updateSpiritFireSound(const Vec3& cam);
    // The fire's lights (docs/FIRE-RENDER.md F3): the front's cells
    // aggregated per 8 m tile into point lights, the nearest tiles first,
    // inserted ahead of the world's lights in the frame's list (the
    // clustered path and the GI take them like any other).
    static constexpr f32 kFireLightTile = 8.0f;
    void extractFireLights(render::RenderSnapshot& out);
    // E3.c — the fire is dangerous: actors standing on a glowing cell take
    // the spirit's contactEffect (an ignition buildup, §2.9: the ONLY path
    // to their attributes) every contactPeriod; wooden props (StaticForm
    // surfaceMaterial "wood") heat up in the fire, burn for their
    // material's fuel, light the ground around them and are gone for good
    // (disabled in the save layer, like a picked-up item).
    void applyFireContact(f32 dt);
    // One actor's contact with fire this frame: `inFire` accumulates its
    // clock toward contactPeriod, then the spirit's contactEffect
    // (buildup) and contactDamage (typed, through applyDamage) land.
    void fireTouch(ecs::Entity entity, f32& clock, f32 dt, bool inFire);
    void updateSpiritFireProps();
    f32 playerFireClock { 0.0f };
    std::unordered_map<u64, f32> npcFireClocks;
    // Actors standing in the flame jet THIS frame: the contact pass reads
    // it beside the ground's own fire (it would otherwise reset their
    // contact clock every job, the ground under them being unlit).
    std::unordered_set<u64> flameJetTouched;
    std::unordered_map<u64, f32> propFireHeat; // entity id -> heat 0..1
    struct BurningProp {
        ecs::Entity entity;
        f32 remaining { 0.0f };
        u32 flames { 0 };
        u32 sparks { 0 };
        u32 smoke { 0 };
        bool lit { false }; // the ground around it took its spark
    };
    vector<BurningProp> burningProps;
    static constexpr f32 kPropHeatRate = 0.4f; // per second in full fire
    // E3.e — the trees of the scatter in the fire: one state per tree
    // (keyed by its quantized base), heated by the burning cells around
    // its trunk, burning with flames, its canopy going through the mask's
    // B channel (the tree shader's leaf fall), regrowing after. Never
    // removed.
    struct TreeFireEntry {
        Vec3 at { 0.0f };
        f32 scale { 1.0f };
        world::TreeFire state;
        u32 flames { 0 };      // the trunk and branches
        u32 crownFlames { 0 }; // the canopy
        u32 sparks { 0 };
        u32 smoke { 0 };
        f32 emberClock { 0.0f };
    };
    std::unordered_map<u64, TreeFireEntry> treeFires;
    vector<u8> fireCanopy; // the mask's B channel, composed per landed job
    f32 fireLandClock { 0.0f }; // sim seconds since the last landed job
    void updateSpiritFireTrees(f32 landDt);
    static constexpr f32 kTreeFireReach = 160.0f;

    static u64 treeKey(const Vec3& at) {
        const i64 x = static_cast<i64>(std::llround(at.x * 4.0f));
        const i64 z = static_cast<i64>(std::llround(at.z * 4.0f));
        return (static_cast<u64>(x) << 32) ^ (static_cast<u64>(z) & 0xffffffffu);
    }
    // Control x Fire, the firebrand: a flame at the aim that lights the
    // ground under it while the key is held (upkeep like the water hold).
    struct SpiritBrand {
        f32 heat { 1.0f };
        f32 radius { 1.0f };
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        f32 pulseClock { 0.0f };
        u32 emitter { 0 };
        core::Guid ability;
    };
    std::optional<SpiritBrand> spiritBrand;
    void updateSpiritBrand(f32 dt);
    void endSpiritBrand();
    static constexpr f32 kBrandPulse = 0.25f;
    // Destroy x Fire on self, held: the fire ward — a douse repeated by
    // every fire job around the caster, who feels no flame meanwhile.
    struct SpiritGlobe {
        f32 radius { 6.0f };
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        u32 emitter { 0 };
        core::Guid ability;
    };
    std::optional<SpiritGlobe> spiritGlobe;
    void updateSpiritGlobe(f32 dt);
    void endSpiritGlobe();
    // Create x Fire as a stream, held: the flame jet — the fire's
    // counterpart of the water jet, short and straight: flames stream
    // from the hand along the aim, the ground under the cone catches
    // (props and trees through the field), whoever stands in it burns.
    struct SpiritFlameJet {
        f32 heat { 1.0f };
        f32 radius { 1.0f };
        f32 range { 10.0f };
        f32 costPeriod { 0.5f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        f32 pulseClock { 0.0f };
        u32 emitter { 0 };
        core::Guid ability;
    };
    std::optional<SpiritFlameJet> spiritFlameJet;
    void updateSpiritFlameJet(f32 dt);
    void endSpiritFlameJet();
    static constexpr f32 kFlameJetPulse = 0.25f;
    // E4.b — the wind spirit. The scene's wind FIELD: the weather's
    // global wind (direction and strength x kWindSpeedPerStrength m/s)
    // plus the placed gusts (Wind SpiritSources) and the breath's carried
    // gust; the fire job, the particles and the player's body read it.
    render::terrain::WindField windField;
    static constexpr f32 kWindSpeedPerStrength = 5.0f; // m/s at weather strength 1
    static constexpr f32 kPlayerWindPush = 0.6f;       // of the gusts' speed
    static constexpr f32 kFloaterWindDrift = 0.08f;    // of the wind, on a floating prop
    // A water jet's lump passing through an actor shoves him along its
    // flight: this many m/s of shove gained per second in the stream,
    // capped (the stream flows at ~24 m/s; a character is not a leaf).
    static constexpr f32 kWaterJetShoveRate = 24.0f;
    static constexpr f32 kWaterJetShoveMax = 6.0f;
    void updateSpiritWind(f32 dt);
    // The breath (Create x Wind as a stream, held).
    struct SpiritBlow {
        f32 speed { 12.0f };
        f32 radius { 6.0f };
        f32 range { 20.0f };
        f32 costPeriod { 0.5f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        u32 emitter { 0 };
        core::Guid ability;
    };
    std::optional<SpiritBlow> spiritBlow;
    void updateSpiritBlow(f32 dt);
    void endSpiritBlow();
    // The weather's wind under a spell: calmed (Destroy x Wind on self,
    // held) or steered where the caster faces (Control x Wind on self,
    // held). The base is what the weather set before the spell.
    struct SpiritWindHold {
        bool calm { false };
        bool direct { false };
        // Released: the wind eases back to the weather's over ~1 s
        // before the hold ends (a recast cancels the release).
        bool releasing { false };
        f32 calmFactor { 1.0f };
        f32 currentDeg { 0.0f }; // the eased heading (direct)
        f32 costPeriod { 1.0f };
        f32 costClock { 0.0f };
        f32 upkeepScale { 0.25f };
        core::Guid ability;
    };
    std::optional<SpiritWindHold> spiritWindHold;
    f32 windBaseStrength { 1.0f };
    f32 windBaseDirectionDeg { 20.0f };
    void updateSpiritWindHold(f32 dt);
    static constexpr f32 kWindHoldEase = 0.33f; // s; ~95 % of the way in a second
    void endSpiritWindHold();
    void resetSpiritWind();
    static constexpr f32 kFlameJetSpeed = 14.0f;     // m/s, the flames' travel
    static constexpr f32 kFlameJetHalfAngle = 0.28f; // radians, the cone
    // The wall gesture: the press spot, the release spot builds the ridge.
    struct SpiritLine {
        Vec3 start { 0.0f };
        f32 height { 0.0f };
        f32 halfWidth { 1.5f };
    };
    std::optional<SpiritLine> spiritLine;
    void updateSpiritLine();
    // Understand x Earth: the ground under the aim, same HUD block.
    void castGroundReading(const Vec3& at, f32 seconds, bool live);
    enum class ReadingKind : u8 { Water, Ground, Fire };
    // Understand x Fire: the cell under the aim (cold / heating / burning /
    // burnt and regrowing), its fuel, the nearest front.
    void castFireReading(const Vec3& at, f32 seconds, bool live);
    ReadingKind spiritReadingKind { ReadingKind::Water };
    // Ground rising into characters throws them (the earth spirit's lift
    // curve, SpiritForm Earth). ONE mechanism at the ONE place the ground
    // changes — the republishTerrain funnel: every actor standing in a
    // changed chunk is sampled before and after the overlay swap; the
    // rise is the throw. Spells, previews and the sculpt tool alike.
    void throwActorsOnGroundRise(
        const std::function<void()>& swapOverlay,
        const std::vector<u64>& changedChunks);
    // Understand x Water: the reading shown by the HUD for a while
    // (world/spirit/WaterReading, formatted here from the loc keys).
    vector<str> spiritReading; // one line per HUD slot (kReadingLines)
    f32 spiritReadingSeconds { 0.0f };
    // A channeled reading follows the aim while Q is held and closes on
    // release (the timed one just fades).
    bool spiritReadingLive { false };
    static constexpr u32 kReadingLines = 6;
    void castWaterReading(const Vec3& at, f32 seconds, bool live);
    void updateSpiritReading(f32 dt);
    // The control spell's held volume (world/spirit/SpiritJets
    // SpiritHold): drawn at the aim while Q is held, dropped on release.
    std::optional<world::SpiritHold> spiritHold;
    void updateSpiritHold(f32 dt);
    void releaseSpiritHold(bool drop);
    // The spell book (draft UI): every ability carrying a SpellForm, in
    // editorId order; the wheel cycles it, the HUD names the current one.
    vector<const gameplay::AbilityForm*> spellBook;
    void buildSpellBook();
    void cycleSpell(i32 direction);
    str currentSpellName() const;
    // The gestures' water as geometry (jet tubes, the carried blob) into
    // the snapshot — drawn with the placed-volume water look.
    void extractSpiritWater(render::RenderSnapshot& out) const;
    // The aimed ground point: the eye ray against physics, rejected when
    // the hit sits on a prop rather than the terrain. nullopt = nothing.
    std::optional<Vec3> aimGround() const;
    // Q in Play: the geometric pre-checks (aim, dry ground) then the
    // ability activation — cost/cooldown are its effects (§6), its
    // script places the source.
    void castSpirit();
    // A compiled SpellForm (docs/SPELLS.md) into world actions; `aimedAt`
    // is the pre-checked ground spot for point spells.
    void executeSpell(const world::SpellSpec& spell,
                      const std::optional<Vec3>& aimedAt);
    // `self` for the player's ability scripts (the trigger idiom, with
    // the liveness handle so a coroutine survives archetype moves).
    script::ScriptContext playerScriptContext();
    f32 mapPrefetchCooldown { 0.0f };
    // Boot/mode-switch camera: sandbox -> the probed start, story -> the
    // NPC-side viewpoint.
    void placeStartCamera();
    // farPlane follows the live terrain view radius (slider up to 45
    // chunks = 2880 m; the old fixed 1600 clipped everything past it).
    void updateCameraFarPlane();

    // Stutter hunt: per-block frame breakdown, logged on spikes > 25 ms.
    core::FrameProbe frameProbe;

    // Consumed (queued into the SaveController, file deleted) on the
    // first onEnter — see the constructor.
    str bootLoadSlot;
    // Dev boot override, from the environment: MEADOWS_BOOT = "story" |
    // "sandbox" enters Play by itself the moment the warmup reveals the
    // world (the main-menu click, scripted), MEADOWS_BOOT_SECONDS = N
    // quits N seconds later — the smoke of the REAL scene, no hand on
    // the mouse (the headless suite never reaches the GPU paths).
    bool bootPlayPending { false };
    bool bootSandbox { false };
    f32 bootQuitSeconds { 0.0f };
    f32 bootQuitClock { 0.0f };

    // World warmup — the ONE state machine behind every loading veil
    // (boot, sandbox entry, travel, the spectator catch-up). Phases run
    // strictly in order so consumers never race the generation:
    //   BakeRing    bake/publish every tile of the ring at `target`
    //   PlaceSpawn  validate/relocate the spawn ONCE, on the FINAL
    //               baked+water world (sandbox boot only)
    //   BuildScene  let meshes/scatter/caches converge on that world
    //   Reveal      fade the veil
    // Soft mode (spectator catch-up) shows a light veil instead of the
    // black shroud and cancels itself if the camera speeds off.
    enum class WarmupPhase : u8 {
        Idle,
        BakeRing,
        PlaceSpawn,
        BuildScene,
        Reveal,
    };
    // `spawnSearchRadius`: how far the wet-spawn validation may
    // relocate. Mode entries scout up to 6 km; a TRAVEL arrival is an
    // authored point — it may only snap locally, never be re-scouted
    // kilometers away.
    void armWarmup(const Vec3& target, bool placeSpawn, bool soft,
                   f32 minSeconds = 0.0f,
                   f32 spawnSearchRadius = 6000.0f);
    void updateWarmup(f32 dt);    // end of update(): phase machine + veil state
    void finalizeSandboxSpawn();  // the single-shot spawn validation
    WarmupPhase warmupPhase { WarmupPhase::Idle };
    bool pendingPlayEntry { false }; // "Play" clicked mid-warmup
    Vec3 warmupTarget { 0.0f };
    bool warmupPlaceSpawn { false };
    bool warmupSoft { false };
    u32 warmupFrames { 0 };
    u32 warmupPeakPending { 0 };  // BuildScene high-water mark
    f32 warmupProgress { 0.0f };  // monotone within one warmup
    f32 warmupElapsed { 0.0f };   // wall time under the veil
    f32 warmupSpawnRadius { 6000.0f }; // wet-relocation bound (armWarmup)
    // Minimum veil time (map travel asks ~1 s so the crossing READS as
    // a transition even with both maps warm; 0 everywhere else).
    f32 warmupMinSeconds { 0.0f };
    // Spectator soft veil re-arm cooldown: a hover near the map rim
    // completes its clamped ring instantly — without the cooldown the
    // veil arms and reveals every few frames (border flicker).
    f32 softVeilCooldown { 0.0f };
    f32 loadingGateAlpha { 0.0f };
    f32 loadingGateShown { 0.0f }; // eased display value (bar/label)
    Vec3 warmupLastCamPos { 0.0f }; // spectator speed estimate

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
    // R3 (engine/terrain/WaterQuery.hpp): the frame's gameplay water
    // query — the sim snapshot is authoritative inside its trusted rect
    // only while it is what the eye SEES (valid, not settling); baked
    // bodies + sea everywhere else. Build at call time, use same-frame.
    render::terrain::WaterQuery makeWaterQuery();
    // Riding v1 (tech proof): while mounted the capsule is
    // destroyed and RideController runs INSTEAD of PlayerController —
    // one if/else at the update call site, PlayerController untouched.
    // Mount/dismount arrive through the interaction closure; travel and
    // mode exits force a dismount first.
    RideController rideController;
    RideContext makeRideContext();
    // Refreshed each frame at the equipMods site; gates jump/sprint
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
