#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/anim/Anim.hpp"          // anim::GraphDesc/GraphInstance/Pose/Skeleton
#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp"          // ecs::Entity, ecs::World
#include "engine/rhi/Rhi.hpp"            // rhi::*Handle
#include "game/scenes/NpcCombatController.hpp" // the in-combat half
#include "game/scenes/NpcScheduleController.hpp" // the peaceful-life half
#include "game/scenes/NpcSpawner.hpp"    // RigData + the Forms->NPC build
#include "gameplay/ability/GameplayTags.hpp" // gameplay::GameplayTag
#include "gameplay/combat/CombatAi.hpp"      // gameplay::CombatMove (brains)

namespace core {
class Rng;
}
namespace rhi {
class Device;
}
namespace render {
struct TerrainParams;
}
namespace data {
class FormDatabase;
struct WeaponForm;
}
namespace assets {
class AssetDatabase;
}
namespace phys {
class PhysicsWorld;
class CharacterBody;
}
namespace world {
class TerrainNavigator;
class SpatialIndex;
}
namespace script {
class Vm;
}
namespace gameplay {
class DerivedStatRegistry;
struct StatsTuningForm;
class EventBus;
class CueRegistry;
struct GameClock;
class FurnitureOccupancy;
struct AiPackageForm;
struct AbilityForm;
} // namespace gameplay

namespace render {
struct RenderSnapshot; // engine/render/SceneView.hpp — the extract target
}

namespace game {

class ProjectileDirector; // Archer NPCs

// [cpp-tuning] — the sword grip corrections (see the definitions
// in NpcDirector.cpp). ONE definition shared by the extract (the drawn
// blade) and the combat controller (the hit segment): the blade that hits
// stays the blade you see.
extern const Mat4 kSwordGrip;
extern const Mat4 kSwordGuardGrip;

// Per-NPC runtime state (non-reflected — docs/HORIZONTAL-PASS.md §H5).
// uptr in the owning vector: the
// GraphInstance references Npc::graph — addresses must survive vector growth.
// The DRAW state (palette SSBO, model UBO, bind groups) moved behind
// the snapshot seam — the renderer owns it, keyed by entity id. The director
// keeps the skin GEOMETRY (vertices/indices — residency, built once per NPC)
// whose handles the snapshot carries, plus the CPU pose it extracts.
struct Npc {
    ecs::Entity entity;
    // The ActorForm's editorId — logs and debug views name the NPC
    // instead of an entity id.
    str editorId;
    const RigData* rig { nullptr };
    anim::GraphDesc graph; // owns the clips; `anim` references it
    uptr<anim::GraphInstance> anim;
    anim::Pose pose;
    vector<Mat4> palette;
    Vec4 tint { 1.0f };
    rhi::BufferHandle vertices {};
    rhi::BufferHandle indices {};
    u32 indexCount { 0 };
    // Patrol: walk to patrolPoints[target], pause, swap ends.
    u32 target { 0 };
    f32 pauseTimer { 0.0f };
    f32 yaw { 0.0f };
    f32 speed { 0.0f }; // smoothed horizontal speed -> anim param

    // Schedule-driven life (replaces the patrol when the
    // ActorForm carries a schedule; patrol stays the fallback).
    core::Guid schedule {};
    bool scheduleInterrupted { false }; // combat/dialogue override edge
    i32 lastEvaluatedSlot { -1 }; // 10-game-minute re-eval granularity
    const gameplay::AiPackageForm* activePackage { nullptr };
    core::Guid activeLocation {};
    str intentReason; // the debug view's "why"
    vector<Vec3> path;
    u32 pathIndex { 0 };
    f32 repathTimer { 0.0f };
    f32 wanderTimer { 0.0f };
    bool sitting { false };  // drives the sitGate anim gate below
    bool furnitureClaimed { false };
    // The furniture's GAS effect (infinite while seated; removed
    // by id on release) and the claimed POINT's anim gate ("State." +
    // FurniturePointForm.animTag — no more hardcoded State.Sitting).
    u32 furnitureEffectId { 0 };
    str sitGate { "State.Sitting" };

    // Combat.
    bool hostile { false }; // ActorTagForm child "Faction.Bandits"
    bool guard { false };   // D2: "Faction.VillageGuard" — hostile while Wanted
    bool dead { false };    // mirrors the GAS State.Dead tag
    // Mirrors State.Downed (an active follower at 0 HP —
    // kneeling, out of the fight, revivable). Same idiom as `dead` above:
    // the director mirrors the tag each frame, everything game-side (the
    // [E] prompt, combat targeting, the party frame) reads the bool.
    bool downed { false };
    // Mirrors "MeleeSwing in flight" for the State.Attacking anim
    // gate (same idiom as `sitting`/`dead` above — the tag-check callback
    // reads these, it never touches gameplay types).
    bool attacking { false };
    // The guard raised between swings (rolled once per window on
    // the engine RNG; mirrored onto the State.Blocking tag).
    bool blocking { false };
    f32 attackCooldown { 0.0f };
    // Grit from the ActorForm — flees below (1 - courage) health.
    f32 courage { 0.75f };
    // ActorForm.age (0 = ageless). Folded per tick into the
    // character mods (gameplay::foldAgeModifiers — the equipmentMods
    // StatModifiers channel): two < 1 multipliers, physical and mental.
    f32 age { 0.0f };
    // Brain script (docs/BOSS-SCRIPTING.md): Lua decides the combat move
    // on low-frequency ticks; empty = the C++ chooseCombatMove. The key
    // is the ActorForm guid (the Vm caches ONE compiled decide per form).
    str brainScript;
    core::Guid brainKey;
    f32 brainTimer { 0.0f };
    std::optional<gameplay::CombatMove> brainMove;
    // The move currently executed (nullopt out of combat) — the combat
    // controller logs its TRANSITIONS with the health fraction, so "the
    // archer flees" vs "the archer holds his bow band" is one log read
    // (a strafe back to reach 12 looks like a flee from the outside).
    std::optional<gameplay::CombatMove> combatMove;
    // The adopted combat TARGET (a follower defending the
    // player, a hostile fighting a follower back). Unset = the pre-
    // default (hostiles hunt the player by perception). Written by the
    // aggro handler (FollowerController, gameplay::adoptOnHit), cleared
    // on the target's OnDeath. RUNTIME ONLY — never saved; re-acquired
    // after load from the next landed hit.
    ecs::Entity combatTarget {};

    // Anim events land HERE from the GraphInstance sink
    // (set at creation, before any EventBus exists for the capture) and
    // are drained onto the bus each update — hit windows and
    // footsteps (C4b) consume them from there.
    vector<str> pendingAnimEvents;

    // The hand bone index ("hand_r", -1 = rig has none) —
    // hostiles carry the VISIBLE weapon there (blade-touch combat).
    i32 handJoint { -1 };
    // Drawn only while combat says so (update mirrors it, extract reads
    // it — a calm bandit keeps the club on his belt).
    bool weaponDrawn { false };
    // Set by moveNpcDirect each frame it steers (strafe/flee — PATHLESS
    // movement): the idle speed decay must not eat the anim speed of an
    // NPC that is very much running.
    bool steered { false };
    // The EQUIPPED weapon's model guid, resolved in update() where the
    // FormDatabase lives (extract has no forms access).
    core::Guid weaponModel;
    // The first Faction.* tag — what the OnDeath event carries
    // (quest kill filters, crime factions).
    gameplay::GameplayTag factionTag {};

    // The class's combat style ("melee", "healer"... —
    // FollowerClassForm.combatStyle, resolved at build) drives how the
    // follower USES his special power in combat; empty = default (self).
    str combatStyle;
    // Bounds the tryActivate call rate for the power (the ability's own
    // cost/cooldown effects are the real gate — tryActivate refuses).
    f32 powerRetryTimer { 0.0f };
};

// The scene systems the NPC subsystem touches, bundled so the whole NPC
// director (build / AI / schedule / combat / draw) is decoupled from
// LandscapeScene. The scene rebuilds it each call from its own
// members — cheap: references plus a few scalars/handles. Mirrors
// EditorContext / StreamingContext.
struct NpcContext {
    ecs::World& world;
    data::FormDatabase& forms;
    assets::AssetDatabase& assetDb;
    const render::TerrainParams& terrainParams;
    gameplay::GameplayTagRegistry& gameTags;
    const gameplay::DerivedStatRegistry& derivedStats;
    const gameplay::StatsTuningForm& statsTuning;
    gameplay::EventBus& eventBus;
    gameplay::GameClock& gameClock;
    gameplay::FurnitureOccupancy& furnitureOccupancy;
    world::TerrainNavigator* navigator;
    phys::PhysicsWorld* physics;
    ecs::Entity playerEntity;
    phys::CharacterBody* player;
    bool playMode;                     // mode == Play (combat hunts the player)
    const data::WeaponForm* banditWeapon;
    // The shared melee attack ability — NPCs pay the same energy
    // cost / cooldown effects as the player (§6).
    const gameplay::AbilityForm* attackAbility;
    // Combat decision rolls (guard chance) — the seeded engine
    // RNG, §8: saves/replays stay reproducible.
    core::Rng& combatRng;
    // Brain scripts (docs/BOSS-SCRIPTING.md): the ONE shared Lua VM;
    // null = every actor runs the C++ brain.
    script::Vm* vm;
    // Feedback cues (hit/block/parry/death) — may be null (tests).
    gameplay::CueRegistry* cues;
    // Where an ARCHER's arrows go (weapon projectileSpeed > 0).
    ProjectileDirector* projectiles;
    bool godMode;
    f32 timeSeconds;                   // cosmetic wander hash (not gameplay RNG)
    // The scene's per-frame actor snapshot — radius
    // queries (the faction shout) go through it, not a full-list sweep.
    const world::SpatialIndex* actorIndex;
    // Schedule interruption: the actor the player is TALKING
    // to right now — set only while the DialogueRunner is active (the
    // QuestDirector partner alone is never cleared on close). Invalid =
    // no dialogue open.
    ecs::Entity dialoguePartner {};
};

// The whole Forms-driven NPC subsystem, extracted from LandscapeScene:
// owns the NPC list and the skinned pipeline; builds newcomers on
// cell changes (NpcSpawner, which owns the rig cache), runs their
// AI/schedule/combat each frame (NpcCombatController /
// NpcScheduleController), and draws them. The scene still reads the
// list (npcs()) for player attack/crime, the shadow caster pass, the debug
// UI, the editor pick and the console.
class NpcDirector {
public:
    // Cell streaming makes NPC entities come and go: prune dead ones (freeing
    // their GPU state) and build newcomers — delegated to NpcSpawner,
    // which fills the director-owned lists below.
    void refreshNpcs(
        rhi::Device& device, const NpcContext& ctx,
        const std::function<void(ecs::Entity, const core::Guid&)>&
            finalizeActorSpawn);

    // Per frame: character tick, schedule, path, combat, anim pose.
    void update(f32 dt, const NpcContext& ctx);

    // Hearing: routes an OnNoise position to every perceiver (the
    // scene subscribes it on the EventBus). `loudness` scales each
    // perceiver's hearing radius — a sneaked step carries half as far.
    void onNoise(const Vec3& position, f32 loudness = 1.0f);

    // Fills snapshot.skinned (copied pose + resolved geometry
    // handles) — the renderer draws ONLY from the packet. Called after
    // update() so the extract carries this frame's pose.
    void extract(render::RenderSnapshot& out) const;

    // onExit teardown: destroy every NPC's skin geometry and drop the
    // caches, so a re-enter starts clean.
    void teardown(rhi::Device& device);

    // The scene reads/mutates the list directly (shadow caster pass creates
    // per-NPC caster groups; player attack / crime / UI iterate it).
    std::vector<uptr<Npc>>& npcs() { return npcs_; }
    const std::vector<uptr<Npc>>& npcs() const { return npcs_; }
    Vec3 characterSpot() const { return characterSpot_; } // first NPC (teleport)

    // Per-faction crime: an actor entity's Faction.* tag
    // (invalid for the player / an unknown entity) — payFine settles the
    // arresting guard's faction through this.
    gameplay::GameplayTag factionOf(u64 entityId) const {
        const Npc* npc = findNpc(entityId);
        return npc ? npc->factionTag : gameplay::GameplayTag {};
    }

private:
    // Entity id -> Npc record, for mapping SpatialIndex hits back to
    // the director's structs. Rebuilt whenever npcs_ changes (refreshNpcs
    // is the only mutation point; uptr keeps the pointers stable).
    Npc* findNpc(u64 entityId) const;

    // The three halves of the subsystem — build (Forms -> NPC, owns
    // the rig cache), combat, peaceful life. The director stays the
    // orchestrator and keeps owning the lists below.
    NpcSpawner spawner_;
    NpcCombatController combat_;
    NpcScheduleController schedule_;

    vector<uptr<Npc>> npcs_;
    std::unordered_map<u64, Npc*> npcByEntity_;
    vector<Vec3> patrolPoints;   // grounded "patrol" marker positions
    Vec3 characterSpot_ { 0.0f }; // first NPC position (teleport target)
    // Per-extract scratch for anim::modelMatrices (weapon attach).
    mutable vector<Mat4> jointScratch;
};

} // namespace game
