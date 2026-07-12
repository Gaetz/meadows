#pragma once

#include <unordered_map>

#include <glm/glm.hpp> // Vec3 by value (Defines only forward-declares glm)

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity (the É2 combat target)

namespace data {
struct WeaponForm;
}
namespace world {
enum class AwareState : u8; // world/ai/Perception.hpp
struct Transform;
}

namespace game {

struct Npc;
struct NpcContext;
class NpcScheduleController;

// FOLLOWERS É2: the frame's combat TARGET — the player by default (the
// pre-É2 behavior, byte-for-byte reads), or the adopted entity when
// Npc.combatTarget is set (a follower defending the player, a hostile
// fighting a follower back). position == feet today (both camps are
// terrain-grounded); crouched only ever means the sneaking player.
struct CombatTarget {
    ecs::Entity entity {};
    Vec3 position { 0.0f };
    Vec3 feet { 0.0f };
    bool crouched { false };
    bool alive { false };
};

// R4: the in-combat half of the NPC frame, split out of NpcDirector —
// perception (vision cone + LOS -> aware state, the call-for-help shout),
// the stagger override, the engagement decision (gameplay/combat/CombatAi
// or a Lua brain) and its execution, then — once the director evaluated
// the pose — the swing machine and the blade-touch hit on the target
// (player or adopted entity — É2).
class NpcCombatController {
public:
    // B5→B2: hostile actors perceive the player — vision cone + LOS
    // feed the Perception state machine; Alert hunts, Searching walks
    // to the last known position and gives up on a timeout. D2: a
    // guard turns hostile while the player carries a bounty
    // (tag-based — the relations table stays a later pass).
    // Returns true when combat overrode the schedule this frame.
    // `schedule` stands the NPC up (releaseFurniture, D1); `npcByEntity`
    // maps callForHelp's SpatialIndex hits back to Npc records (R3).
    bool update(f32 dt, const NpcContext& ctx, Npc& npc,
                const data::WeaponForm* npcWeapon, bool playerSneaking,
                NpcScheduleController& schedule,
                const std::unordered_map<u64, Npc*>& npcByEntity);

    // P0 A3/A4: the swing machine + the blade-touch hit (the SAME
    // MeleeSwing code path as the player), plus the A5 guard roll/clock
    // and its State.Blocking mirror. Runs for every living NPC, in
    // combat or not, AFTER the director evaluated this frame's pose
    // (the hit segment follows the hand joint). É2: the defender is the
    // combat target — `npcByEntity` resolves an NPC defender's record.
    void updateSwing(f32 dt, const NpcContext& ctx, Npc& npc,
                     const data::WeaponForm* npcWeapon, bool playerSneaking,
                     const std::unordered_map<u64, Npc*>& npcByEntity);

private:
    // The CombatMove executions — one method per move (R4); the switch
    // in update() only dispatches. É2: all of them work on the resolved
    // CombatTarget's position — player or adopted entity alike.
    void strike(const NpcContext& ctx, Npc& npc, world::Transform& transform,
                const data::WeaponForm* npcWeapon, bool swinging,
                bool quiverDry, const Vec3& toTarget,
                const CombatTarget& target);
    // A7: an ARCHER looses an arrow (Strike with a ranged weapon).
    void fireArrow(const NpcContext& ctx, Npc& npc,
                   world::Transform& transform,
                   const data::WeaponForm& npcWeapon, const Vec3& targetPos);
    void strafe(f32 dt, const NpcContext& ctx, Npc& npc,
                world::Transform& transform, const Vec3& toTarget,
                f32 targetDistance, f32 attackRange, f32 reach);
    void flee(f32 dt, const NpcContext& ctx, Npc& npc,
              world::Transform& transform, const Vec3& toTarget,
              f32 targetDistance);
    void approach(f32 dt, const NpcContext& ctx, Npc& npc,
                  world::Transform& transform, bool canSee, bool swinging,
                  const Vec3& targetPos, const Vec3& lastKnownPos,
                  world::AwareState aware);
    // B3: entering Alert shouts — same-faction allies in
    // StatsTuningForm.helpCallRadius get the target position (alertTo).
    void callForHelp(const NpcContext& ctx, const Npc& caller,
                     const Vec3& targetPos,
                     const std::unordered_map<u64, Npc*>& npcByEntity);

    // FOLLOWERS É6: an ACTIVE follower in combat tries his special power
    // (the first granted ability that isn't the shared attack — granted by
    // his class perks). Policy per FollowerClassForm.combatStyle:
    //   "healer"        -> tryActivate the power ON the lowest ally
    //                      strictly below followerHealThreshold (player,
    //                      followers, herself — gameplay::pickHealTarget);
    //   anything else   -> self-cast when engaging ("melee" war cry).
    // The ability's own cost/cooldown effects gate the cadence
    // (tryActivate refuses); powerRetryTimer only bounds the call rate.
    void tryUsePower(f32 dt, const NpcContext& ctx, Npc& npc,
                     const std::unordered_map<u64, Npc*>& npcByEntity);

    // A2: per-strike scratch for anim::modelMatrices (the hit segment).
    vector<Mat4> jointScratch;
};

} // namespace game
