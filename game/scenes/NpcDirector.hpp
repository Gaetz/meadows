#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/anim/Anim.hpp"          // anim::GraphDesc/GraphInstance/Pose/Skeleton
#include "engine/assets/GltfMesh.hpp"    // assets::GltfClip
#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp"          // ecs::Entity, ecs::World
#include "engine/rhi/Rhi.hpp"            // rhi::*Handle
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

namespace game {

struct RenderSnapshot; // game/SceneSubmit.hpp — the extract target
class ProjectileDirector; // A7: archer NPCs

// One rig cache entry per glTF asset (skeleton + its clips). Shared by every
// NPC built from that asset.
struct RigData {
    anim::Skeleton skeleton;
    vector<assets::GltfClip> clips;
};

// Per-NPC runtime state (non-reflected, §H5). uptr in the owning vector: the
// GraphInstance references Npc::graph — addresses must survive vector growth.
// U4-2b: the DRAW state (palette SSBO, model UBO, bind groups) moved behind
// the snapshot seam — the renderer owns it, keyed by entity id. The director
// keeps the skin GEOMETRY (vertices/indices — residency, built once per NPC)
// whose handles the snapshot carries, plus the CPU pose it extracts.
struct Npc {
    ecs::Entity entity;
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

    // Chantier 3 B3: schedule-driven life (replaces the patrol when the
    // ActorForm carries a schedule; patrol stays the fallback).
    core::Guid schedule {};
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
    // P0 D1: the furniture's GAS effect (infinite while seated; removed
    // by id on release) and the claimed POINT's anim gate ("State." +
    // FurniturePointForm.animTag — no more hardcoded State.Sitting).
    u32 furnitureEffectId { 0 };
    str sitGate { "State.Sitting" };

    // Chantier 3 B5/B6: combat.
    bool hostile { false }; // ActorTagForm child "Faction.Bandits"
    bool guard { false };   // D2: "Faction.VillageGuard" — hostile while Wanted
    bool dead { false };    // mirrors the GAS State.Dead tag
    // P0 A3: mirrors "MeleeSwing in flight" for the State.Attacking anim
    // gate (same idiom as `sitting`/`dead` above — the tag-check callback
    // reads these, it never touches gameplay types).
    bool attacking { false };
    // P0 A5: the guard raised between swings (rolled once per window on
    // the engine RNG; mirrored onto the State.Blocking tag).
    bool blocking { false };
    f32 attackCooldown { 0.0f };
    // P0 B3: grit from the ActorForm — flees below (1 - courage) health.
    f32 courage { 0.75f };
    // Brain script (docs/BOSS-SCRIPTING.md): Lua decides the combat move
    // on low-frequency ticks; empty = the C++ chooseCombatMove. The key
    // is the ActorForm guid (the Vm caches ONE compiled decide per form).
    str brainScript;
    core::Guid brainKey;
    f32 brainTimer { 0.0f };
    std::optional<gameplay::CombatMove> brainMove;

    // Chantier P0 C4a: anim events land HERE from the GraphInstance sink
    // (set at creation, before any EventBus exists for the capture) and
    // are drained onto the bus each update — hit windows (A4) and
    // footsteps (C4b) consume them from there.
    vector<str> pendingAnimEvents;

    // Chantier P0 A2: the hand bone index ("hand_r", -1 = rig has none) —
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
    // Chantier 6 A1: the first Faction.* tag — what the OnDeath event carries
    // (quest kill filters, crime factions).
    gameplay::GameplayTag factionTag {};
};

// The scene systems the NPC subsystem touches, bundled so the whole NPC
// director (build / AI / schedule / combat / draw) is decoupled from
// LandscapeScene (audit U4-10). The scene rebuilds it each call from its own
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
    // P0 A3: the shared melee attack ability — NPCs pay the same energy
    // cost / cooldown effects as the player (§6).
    const gameplay::AbilityForm* attackAbility;
    // P0 A5: combat decision rolls (guard chance) — the seeded engine
    // RNG, §8: saves/replays stay reproducible.
    core::Rng& combatRng;
    // Brain scripts (docs/BOSS-SCRIPTING.md): the ONE shared Lua VM;
    // null = every actor runs the C++ brain.
    script::Vm* vm;
    // P0 C2: feedback cues (hit/block/parry/death) — may be null (tests).
    gameplay::CueRegistry* cues;
    // A7: where an ARCHER's arrows go (weapon projectileSpeed > 0).
    ProjectileDirector* projectiles;
    bool godMode;
    f32 timeSeconds;                   // cosmetic wander hash (not gameplay RNG)
};

// The whole Forms-driven NPC subsystem, extracted from LandscapeScene (audit
// U4-10): owns the rig cache, the NPC list, and the skinned pipeline; builds
// newcomers on cell changes, runs their AI/schedule/combat each frame, and
// draws them. The scene still reads the list (npcs()) for player attack/crime,
// the shadow caster pass, the debug UI, the editor pick and the console.
class NpcDirector {
public:
    // Cell streaming makes NPC entities come and go: prune dead ones (freeing
    // their GPU state) and build newcomers. finalizeActorSpawn adds the stats /
    // saved-state / loadout components (shared with the player — stays in the
    // scene), applied deferred (a table move on the locked iteration).
    void refreshNpcs(
        rhi::Device& device, const NpcContext& ctx,
        const std::function<void(ecs::Entity, const core::Guid&)>&
            finalizeActorSpawn);

    // Per frame: character tick, schedule, path, combat, anim pose.
    void update(f32 dt, const NpcContext& ctx);

    // P0 B2 hearing: routes an OnNoise position to every perceiver (the
    // scene subscribes it on the EventBus). `loudness` scales each
    // perceiver's hearing radius — a sneaked step carries half as far.
    void onNoise(const Vec3& position, f32 loudness = 1.0f);

    // U4-2b: fills snapshot.skinned (copied pose + resolved geometry
    // handles) — the renderer draws ONLY from the packet. Called after
    // update() so the extract carries this frame's pose.
    void extract(RenderSnapshot& out) const;

    // onExit teardown: destroy every NPC's skin geometry and drop the
    // caches, so a re-enter starts clean.
    void teardown(rhi::Device& device);

    // The scene reads/mutates the list directly (shadow caster pass creates
    // per-NPC caster groups; player attack / crime / UI iterate it).
    std::vector<uptr<Npc>>& npcs() { return npcs_; }
    const std::vector<uptr<Npc>>& npcs() const { return npcs_; }
    Vec3 characterSpot() const { return characterSpot_; } // first NPC (teleport)

private:
    const RigData* loadRig(const NpcContext& ctx, const core::Guid& asset);
    void destroyNpc(rhi::Device& device, Npc& npc);
    void updateNpcSchedule(const NpcContext& ctx, Npc& npc, f32 hourOfDay);
    bool moveNpcAlongPath(const NpcContext& ctx, Npc& npc, f32 dt,
                          f32 speedScale);
    // B3: pathless steering (strafe orbits, flee) — same stat-driven
    // speed as the path walker, explicit facing (a strafer keeps its
    // eyes on the target, a runner looks where it runs).
    void moveNpcDirect(const NpcContext& ctx, Npc& npc, f32 dt,
                       const Vec3& direction, f32 speedScale, f32 faceYaw);
    // B3: entering Alert shouts — same-faction allies in
    // StatsTuningForm.helpCallRadius get the target position (alertTo).
    void callForHelp(const NpcContext& ctx, const Npc& caller,
                     const Vec3& targetPos);
    // D1: the ONE way out of furniture — frees the occupancy point,
    // removes the furniture's GAS effect, clears the anim gate.
    void releaseFurniture(const NpcContext& ctx, Npc& npc);

    std::unordered_map<core::Guid, RigData> rigCache;
    vector<uptr<Npc>> npcs_;
    vector<Vec3> patrolPoints;   // grounded "patrol" marker positions
    Vec3 characterSpot_ { 0.0f }; // first NPC position (teleport target)
    // A2: per-extract scratch for anim::modelMatrices (weapon attach).
    mutable vector<Mat4> jointScratch;
};

} // namespace game
