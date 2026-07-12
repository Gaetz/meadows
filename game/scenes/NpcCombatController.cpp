#include "game/scenes/NpcCombatController.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "data/forms/CoreForms.hpp"       // data::WeaponForm
#include "data/forms/FormDatabase.hpp"
#include "engine/core/Log.hpp"
#include "engine/core/Rng.hpp"            // A5: NPC guard rolls (§8)
#include "engine/physics/Physics.hpp"     // phys::PhysicsWorld/CharacterBody
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "game/scenes/LineOfSight.hpp"          // hasLineOfSight (R2)
#include "game/scenes/NpcDirector.hpp"          // Npc, NpcContext, kSwordGrip
#include "game/scenes/NpcMovement.hpp"    // moveNpc*, steerBlocked
#include "game/scenes/NpcScheduleController.hpp" // releaseFurniture (D1)
#include "game/scenes/ProjectileDirector.hpp"   // archer NPCs (A7)
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp" // attr, currentValueOf
#include "gameplay/ability/GameplayAbility.hpp" // tryActivate (P0 A3)
#include "gameplay/combat/CombatAi.hpp"         // chooseCombatMove (B3)
#include "gameplay/combat/MeleeStrike.hpp"      // the ONE strike resolution
#include "gameplay/combat/MeleeSwing.hpp"       // the blade-touch swing (A4)
#include "gameplay/combat/Projectile.hpp"       // archer NPCs (A7)
#include "gameplay/event/EventBus.hpp"
#include "gameplay/inventory/Inventory.hpp" // the quiver (A7+)
#include "gameplay/stats/CoreAttributes.hpp"    // StatBlock pieces
#include "gameplay/stats/Damage.hpp"            // gameplay::CombatState
#include "gameplay/stats/EquipmentStats.hpp" // weaponDamageEvent
#include "gameplay/stats/StatsTuning.hpp"
#include "script/Vm.hpp"               // brain scripts (BOSS-SCRIPTING.md)
#include "world/ai/Perception.hpp"      // B2: vision cone + aware states
#include "world/ai/TerrainNavigator.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/SpatialIndex.hpp" // R3: the shared actor snapshot

namespace game {

void NpcCombatController::callForHelp(
    const NpcContext& ctx, const Npc& caller, const Vec3& targetPos,
    const std::unordered_map<u64, Npc*>& npcByEntity) {
    // The shout on the bus first: quests/scripts/mods can listen.
    ctx.eventBus.dispatch({ gameplay::eventKind("CallForHelp"),
                            caller.entity, ecs::Entity {},
                            caller.factionTag });
    if (!caller.factionTag.isValid() || !ctx.actorIndex) {
        return; // no faction, no friends (the index is the scene's)
    }
    // R3 (B1 adopted): the shared actor snapshot answers "who is in
    // earshot"; entity hits map back to the director's Npc records.
    const Vec3 callerPos = caller.entity.get<world::Transform>().position;
    vector<world::SpatialIndex::Entry> inEarshot;
    ctx.actorIndex->queryRadius(callerPos, ctx.statsTuning.helpCallRadius,
                                inEarshot);
    for (const auto& entry : inEarshot) {
        const auto it = npcByEntity.find(entry.entity.id());
        Npc* ally = it != npcByEntity.end() ? it->second : nullptr;
        if (!ally || ally == &caller || ally->dead ||
            !ally->entity.is_alive() ||
            ally->factionTag != caller.factionTag ||
            !ally->entity.has<world::Perception>()) {
            continue;
        }
        world::alertTo(ally->entity.get_mut<world::Perception>(),
                       targetPos);
        ally->sitting = false; // an alerted ally stands up
    }
}

bool NpcCombatController::update(
    f32 dt, const NpcContext& ctx, Npc& npc,
    const data::WeaponForm* npcWeapon, bool playerSneaking,
    NpcScheduleController& schedule,
    const std::unordered_map<u64, Npc*>& npcByEntity) {
    bool wanted = false;
    if (npc.guard && ctx.playerEntity.is_alive()) {
        if (const auto tag = ctx.gameTags.find("Crime.Wanted")) {
            wanted = ctx.playerEntity.get<gameplay::AbilitySystem>()
                         .tags.has(*tag);
        }
    }
    if (!((npc.hostile || wanted) && ctx.playMode && ctx.player)) {
        return false;
    }
    bool inCombat = false;
    auto& transform = npc.entity.get_mut<world::Transform>();
    const auto& npcSys = npc.entity.get<gameplay::AbilitySystem>();
    const Vec3 playerPos = ctx.player->position();
    auto& perception = npc.entity.get_mut<world::Perception>();
    // Vision verdict: cone from the NPC's facing, then the LOS
    // raycast (world geometry only — actors are out of the
    // broadphase anyway). A SNEAKING target is spotted at half
    // range (the sneak skill will drive the factor later), and
    // the LOS aims at the CROUCHED chest — a low wall now hides.
    world::Perception sight = perception;
    if (playerSneaking) {
        sight.viewDistance *= ctx.statsTuning.sneakDetectionFactor;
    }
    const Vec3 facing { std::sin(npc.yaw), 0.0f,
                        std::cos(npc.yaw) };
    bool canSee = world::inViewCone(sight, transform.position,
                                    facing, playerPos);
    if (canSee && ctx.physics) {
        canSee = hasLineOfSight(
            *ctx.physics,
            transform.position + Vec3 { 0.0f, 1.5f, 0.0f },
            playerPos + Vec3 { 0.0f, playerSneaking ? 0.6f : 1.2f,
                               0.0f });
    }
    const world::AwareState wasAware = world::awareState(perception);
    world::updatePerception(perception, canSee, playerPos, dt);
    const world::AwareState aware = world::awareState(perception);
    // B3: entering Alert shouts — a bus event for listeners, and
    // same-faction allies in earshot join the hunt.
    if (aware == world::AwareState::Alert &&
        wasAware != world::AwareState::Alert) {
        callForHelp(ctx, npc, perception.lastKnownPos, npcByEntity);
    }
    // STATS.md §4: a staggered actor can't act, parry or dodge
    // and moves at a crawl — the bandit just STANDS there,
    // reeling (the riposte window the parry earns).
    bool npcStaggered = false;
    if (const auto staggerTag =
            ctx.gameTags.find("State.Staggered")) {
        npcStaggered = npcSys.tags.has(*staggerTag);
    }
    if (npcStaggered) {
        inCombat = true; // reeling still overrides the schedule
        schedule.releaseFurniture(ctx, npc); // D1: knocked off the seat
        npc.blocking = false;
        npc.path.clear();
        npc.attackCooldown -= dt;
    } else if (aware == world::AwareState::Alert ||
               aware == world::AwareState::Searching) {
        inCombat = true;
        schedule.releaseFurniture(ctx, npc); // D1: combat stands him up
        npc.attackCooldown -= dt;
        npc.repathTimer -= dt;
        // A7+: an archer's quiver is REAL — the loadout rolls his
        // arrows and each shot consumes one. Dry (or no Inventory
        // at all) = no ranged option: the reach collapses to melee
        // so the combat brain closes in and clubs with the bow.
        bool quiverDry = false;
        if (npcWeapon && npcWeapon->projectileSpeed > 0.0f &&
            npcWeapon->ammo.isValid()) {
            quiverDry =
                !npc.entity.has<gameplay::Inventory>() ||
                gameplay::itemCount(
                    npc.entity.get<gameplay::Inventory>(),
                    npcWeapon->ammo) <= 0;
        }
        // P0 A6: the engagement distances come from the WEAPON
        // (§5 moddable) — a spear-armed NPC stands off further
        // than a knife mugger, no code change.
        const f32 reach =
            npcWeapon && !quiverDry ? npcWeapon->reach : 2.1f;
        const f32 attackRange = glm::max(reach - 0.3f, 0.8f);
        // P0 A3: a swing in flight roots the NPC (the clip plays
        // out; the blade does the hitting below).
        const bool swinging =
            npc.entity.get<gameplay::MeleeSwing>().phase !=
            gameplay::SwingPhase::Idle;
        Vec3 toPlayer = playerPos - transform.position;
        toPlayer.y = 0.0f;
        const f32 playerDistance = glm::length(toPlayer);
        // B3: the whole behavior choice is ONE sim-pure function
        // (gameplay/combat/CombatAi) — this block only executes
        // the move it returns.
        const f32 maxHealth = glm::max(
            gameplay::currentValueOf(npcSys,
                                     gameplay::attr("maxHealth")),
            1.0f);
        const gameplay::CombatSituation situation {
            playerDistance,
            attackRange,
            reach + 1.0f,
            canSee,
            swinging,
            npc.attackCooldown,
            gameplay::currentValueOf(npcSys,
                                     gameplay::attr("health")) /
                maxHealth,
            npc.courage
        };
        // The C++ brain by default; a brain SCRIPT (Lua, decision
        // tick — never per frame) overrides the move when it
        // returns a valid name. Errors fall back silently
        // (callBrain logs once and drops the script).
        gameplay::CombatMove move =
            gameplay::chooseCombatMove(situation);
        if (!npc.brainScript.empty() && ctx.vm) {
            npc.brainTimer -= dt;
            if (npc.brainTimer <= 0.0f) {
                npc.brainTimer = 0.25f; // [cpp-tuning] ~4 Hz
                script::ScriptContext sctx;
                sctx.entity = npc.entity;
                sctx.attributes =
                    &npc.entity.get_mut<gameplay::AttributeSet>();
                sctx.abilitySystem =
                    &npc.entity.get_mut<gameplay::AbilitySystem>();
                sctx.tags = &ctx.gameTags;
                sctx.forms = &ctx.forms;
                npc.brainMove = gameplay::parseCombatMove(
                    ctx.vm->callBrain(
                        npc.brainKey, npc.brainScript, sctx,
                        situation,
                        aware == world::AwareState::Alert
                            ? "alert"
                            : "searching"));
            }
            if (npc.brainMove) {
                move = *npc.brainMove;
            }
        }
        switch (move) {
        case gameplay::CombatMove::Strike:
            strike(ctx, npc, transform, npcWeapon, swinging, quiverDry,
                   toPlayer, playerPos);
            break;
        case gameplay::CombatMove::Strafe:
            strafe(dt, ctx, npc, transform, toPlayer, playerDistance,
                   attackRange, reach);
            break;
        case gameplay::CombatMove::Flee:
            flee(dt, ctx, npc, transform, toPlayer, playerDistance);
            break;
        case gameplay::CombatMove::Approach:
            approach(dt, ctx, npc, transform, canSee, swinging, playerPos,
                     perception.lastKnownPos, aware);
            break;
        }
    }
    return inCombat;
}

void NpcCombatController::strike(const NpcContext& ctx, Npc& npc,
                                 world::Transform& transform,
                                 const data::WeaponForm* npcWeapon,
                                 bool swinging, bool quiverDry,
                                 const Vec3& toPlayer,
                                 const Vec3& playerPos) {
    npc.path.clear();
    // Face the player and swing.
    npc.yaw = std::atan2(toPlayer.x, toPlayer.z);
    transform.rotation = glm::angleAxis(
        npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    // P0 A3: instant damage became an ability-gated
    // MeleeSwing — the Sword_Attack clip carries the
    // hand, and the blade must TOUCH (updateSwing).
    if (!swinging && npc.attackCooldown <= 0.0f &&
        npcWeapon && ctx.playerEntity.is_alive() &&
        !ctx.godMode) {
        auto& set =
            npc.entity.get_mut<gameplay::AttributeSet>();
        auto& system =
            npc.entity.get_mut<gameplay::AbilitySystem>();
        const bool activated =
            !ctx.attackAbility ||
            gameplay::tryActivate(*ctx.attackAbility, set,
                                  system, set, system,
                                  { ctx.forms,
                                    ctx.gameTags });
        if (activated &&
            npcWeapon->projectileSpeed > 0.0f &&
            !quiverDry && ctx.projectiles) {
            fireArrow(ctx, npc, transform, *npcWeapon, playerPos);
        } else if (activated) {
            gameplay::startSwing(
                npc.entity
                    .get_mut<gameplay::MeleeSwing>());
            // [cpp-tuning] pause between swings.
            npc.attackCooldown = 1.6f;
            npc.blocking = false; // guard drops to strike
        }
    }
}

void NpcCombatController::fireArrow(const NpcContext& ctx, Npc& npc,
                                    world::Transform& transform,
                                    const data::WeaponForm& npcWeapon,
                                    const Vec3& playerPos) {
    // A7: an ARCHER — loose from the chest at
    // the player's chest, with a hair of spread
    // (deterministic combat RNG, §8).
    gameplay::Projectile arrow;
    arrow.position = transform.position +
                     Vec3 { 0.0f, 1.4f, 0.0f };
    Vec3 aim =
        (playerPos + Vec3 { 0.0f, 1.0f, 0.0f }) -
        arrow.position;
    aim = glm::normalize(aim);
    aim.x += (static_cast<f32>(
                  ctx.combatRng.unit()) -
              0.5f) *
             0.06f;
    aim.z += (static_cast<f32>(
                  ctx.combatRng.unit()) -
              0.5f) *
             0.06f;
    arrow.velocity = glm::normalize(aim) *
                     npcWeapon.projectileSpeed;
    arrow.shooter = npc.entity.id();
    arrow.payload = gameplay::weaponDamageEvent(
        npcWeapon, npc.entity.get<gameplay::AbilitySystem>());
    // A7+: the shot spends an arrow from HIS
    // inventory (quiverDry gated above); planted
    // arrows stay loot for whoever walks by.
    arrow.ammoItem = npcWeapon.ammo;
    if (npcWeapon.ammo.isValid() &&
        npc.entity.has<gameplay::Inventory>()) {
        gameplay::removeItem(
            npc.entity
                .get_mut<gameplay::Inventory>(),
            npcWeapon.ammo);
    }
    ctx.projectiles->spawn(arrow);
    npc.attackCooldown = 2.2f; // [cpp-tuning]
    npc.blocking = false;
}

void NpcCombatController::strafe(f32 dt, const NpcContext& ctx, Npc& npc,
                                 world::Transform& transform,
                                 const Vec3& toPlayer, f32 playerDistance,
                                 f32 attackRange, f32 reach) {
    npc.path.clear();
    // Orbit the target: a tangent direction whose side is
    // picked by entity-id parity (stable per NPC — two
    // bandits circle opposite ways) plus a radial drift
    // holding the middle of the weapon band.
    const Vec3 radial =
        toPlayer / glm::max(playerDistance, 1e-3f);
    const f32 side =
        (npc.entity.id() & 1u) != 0u ? 1.0f : -1.0f;
    const f32 band = (attackRange + reach + 1.0f) * 0.5f;
    Vec3 dir =
        glm::cross(Vec3 { 0.0f, 1.0f, 0.0f }, radial) *
            side +
        radial * glm::clamp(playerDistance - band, -1.0f,
                            1.0f) *
            0.6f;
    dir.y = 0.0f;
    const f32 len = glm::length(dir);
    // Wall guard: direct steering skips the navigator,
    // so probe the step — blocked = hold ground (still
    // facing the player).
    if (len > 1e-4f &&
        !steerBlocked(ctx, transform.position,
                      dir / len)) {
        moveNpcDirect(ctx, npc, dt, dir / len, 1.0f,
                      std::atan2(toPlayer.x, toPlayer.z));
    }
}

void NpcCombatController::flee(f32 dt, const NpcContext& ctx, Npc& npc,
                               world::Transform& transform,
                               const Vec3& toPlayer, f32 playerDistance) {
    // Flee through the NAVIGATOR (dev report 2026-07-12:
    // direct steering ran straight through buildings) —
    // path to a spot away from the player, repathed as
    // the flight goes on; obstacles are its job.
    const Vec3 away =
        playerDistance > 1e-3f
            ? -toPlayer / playerDistance
            : Vec3 { std::sin(npc.yaw), 0.0f,
                     std::cos(npc.yaw) };
    // A broken fighter RUNS (dev feel pass): cancel the
    // NPC walk factor so he flees at full jog speed —
    // solidly inside the run anim's threshold.
    const f32 runScale =
        1.0f /
        glm::max(ctx.statsTuning.npcWalkFactor, 0.05f);
    if (npc.pathIndex >= npc.path.size() ||
        npc.repathTimer <= 0.0f) {
        Vec3 spot = transform.position + away * 12.0f;
        spot.y = render::terrain::height(ctx.terrainParams,
                                         spot.x, spot.z);
        const nav::PathResult found =
            ctx.navigator->findPath(
                { transform.position, spot, 2.0f });
        npc.path = found.success ? found.waypoints
                                 : vector<Vec3> {};
        npc.pathIndex = 0;
        npc.repathTimer = 0.8f;
    }
    if (npc.pathIndex < npc.path.size()) {
        moveNpcAlongPath(ctx, npc, dt, runScale);
    } else if (!steerBlocked(ctx, transform.position,
                             away)) {
        // No path (cornered): raw retreat, wall-guarded.
        moveNpcDirect(ctx, npc, dt, away, runScale,
                      std::atan2(away.x, away.z));
    }
}

void NpcCombatController::approach(f32 dt, const NpcContext& ctx, Npc& npc,
                                   world::Transform& transform, bool canSee,
                                   bool swinging, const Vec3& playerPos,
                                   const Vec3& lastKnownPos,
                                   world::AwareState aware) {
    // Hunt the player while seen; investigate the last
    // known position otherwise (B2).
    const Vec3 goal =
        canSee ? playerPos : lastKnownPos;
    Vec3 toGoal = goal - transform.position;
    toGoal.y = 0.0f;
    const f32 goalDistance = glm::length(toGoal);
    if (!swinging && goalDistance > 0.6f) {
        if (npc.repathTimer <= 0.0f) {
            const nav::PathResult found =
                ctx.navigator->findPath(
                    { transform.position, goal, 1.2f });
            npc.path = found.success ? found.waypoints
                                     : vector<Vec3> {};
            npc.pathIndex = 0;
            npc.repathTimer = 1.0f;
        }
        // Alert hurries; a search walks (may be nothing).
        moveNpcAlongPath(ctx, npc, dt,
                         aware == world::AwareState::Alert
                             ? 1.8f
                             : 1.0f);
    } else if (!canSee) {
        // Arrived at the last known spot and no one is
        // here: stand — the patience runs out on its own.
        npc.path.clear();
    }
}

void NpcCombatController::updateSwing(f32 dt, const NpcContext& ctx,
                                      Npc& npc,
                                      const data::WeaponForm* npcWeapon,
                                      bool playerSneaking) {
    // P0 A3/A4: the swing machine + the blade-touch hit (the SAME
    // MeleeSwing code path as the player). The clip's authored
    // HitOpen/HitClose events override the data windows; the hit
    // segment is the VISIBLE blade — world x hand joint x +Y, exactly
    // what extract() draws — against the player capsule.
    auto& swing = npc.entity.get_mut<gameplay::MeleeSwing>();
    if (swing.phase != gameplay::SwingPhase::Idle && npcWeapon) {
        for (const str& name : npc.pendingAnimEvents) {
            gameplay::onSwingAnimEvent(swing, name);
        }
        const gameplay::SwingTiming timing {
            npcWeapon->swingWindup, npcWeapon->swingActive,
            npcWeapon->swingRecovery
        };
        gameplay::updateSwing(swing, dt, timing);
        if (swing.phase == gameplay::SwingPhase::Idle) {
            // A5: the guard window between swings — ONE roll per
            // window, on the seeded engine RNG (§8).
            npc.blocking = ctx.combatRng.chance(
                static_cast<f64>(ctx.statsTuning.npcBlockChance));
        }
        if (swing.phase == gameplay::SwingPhase::Active &&
            npc.handJoint >= 0 && ctx.playMode && ctx.player &&
            ctx.playerEntity.is_alive() && !ctx.godMode) {
            const auto& transform = npc.entity.get<world::Transform>();
            const Mat4 world =
                glm::translate(Mat4 { 1.0f }, transform.position) *
                glm::mat4_cast(transform.rotation);
            anim::modelMatrices(npc.rig->skeleton, npc.pose,
                                jointScratch);
            const Mat4 hand =
                world *
                jointScratch[static_cast<size_t>(npc.handJoint)] *
                kSwordGrip;
            const Vec3 grip { hand[3] };
            const Vec3 bladeDir = glm::normalize(Vec3 { hand[1] });
            const Vec3 tip =
                grip + bladeDir * (npcWeapon->bladeLength *
                                   npcWeapon->hitTolerance);
            const Vec3 feet = ctx.player->position();
            // Dodge i-frames: State.Dodging means the blade passes
            // through — and does NOT register, so the same Active
            // window can still connect once the i-frames expire.
            bool dodging = false;
            if (const auto dodgeTag =
                    ctx.gameTags.find("State.Dodging")) {
                dodging = ctx.playerEntity
                              .get<gameplay::AbilitySystem>()
                              .tags.has(*dodgeTag);
            }
            // A crouched player is half the target (sneak rule).
            if (!dodging &&
                gameplay::segmentHitsActor(grip, tip, feet,
                                           playerSneaking) &&
                gameplay::registerStrike(swing,
                                         ctx.playerEntity.id())) {
                // The exchange rules (crit window, guard cone,
                // perfect parry, events, cues) live in ONE place,
                // resolveMeleeStrike, shared with the player side.
                gameplay::StatBlock defender {
                    ctx.playerEntity.get_mut<gameplay::CoreAttributes>(),
                    ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
                    ctx.playerEntity.get_mut<gameplay::AbilitySystem>(),
                    ctx.playerEntity.get_mut<gameplay::CombatState>()
                };
                gameplay::StatBlock attacker {
                    npc.entity.get_mut<gameplay::CoreAttributes>(),
                    npc.entity.get_mut<gameplay::AttributeSet>(),
                    npc.entity.get_mut<gameplay::AbilitySystem>(),
                    npc.entity.get_mut<gameplay::CombatState>()
                };
                const auto& playerT =
                    ctx.playerEntity.get<world::Transform>();
                const gameplay::StrikeGeometry geo {
                    transform.position, playerT.position,
                    playerT.rotation * Vec3 { 0.0f, 0.0f, 1.0f },
                    ctx.playerEntity.get<gameplay::MeleeSwing>()
                        .guardSeconds,
                    feet + Vec3 { 0.0f, 1.2f, 0.0f }
                };
                const gameplay::StrikeContext strikeCtx {
                    ctx.gameTags, ctx.derivedStats, ctx.statsTuning,
                    &ctx.eventBus, ctx.cues
                };
                const gameplay::StrikeOutcome outcome =
                    gameplay::resolveMeleeStrike(
                        attacker, defender, npc.entity,
                        ctx.playerEntity,
                        gameplay::weaponDamageEvent(*npcWeapon,
                                                    attacker.system),
                        geo, strikeCtx);
                if (outcome.guard.perfect) {
                    LOG_INFO("PERFECT PARRY — bandit poise -{}{}",
                             ctx.statsTuning.perfectParryPosture,
                             outcome.riposte.staggered
                                 ? " (STAGGERED!)" : "");
                } else {
                    LOG_INFO("Bandit's blade lands: {:.0f} damage{}{}",
                             outcome.damage.healthDamage,
                             outcome.guard.caught ? " (blocked)" : "",
                             outcome.damage.staggered
                                 ? " (staggered!)" : "");
                }
            }
        }
    }
    npc.attacking = swing.phase != gameplay::SwingPhase::Idle;
    // A5+: the guard clock — a player hit landing while this guard
    // is FRESH gets perfect-parried (applyHit reads guardSeconds).
    gameplay::tickGuard(swing, npc.blocking, dt);
    // A5: mirror the guard onto the §6 tag vocabulary — the damage
    // paths (player applyHit, future sources) read State.Blocking,
    // never the Npc struct.
    gameplay::syncStateTag(
        npc.entity.get_mut<gameplay::AbilitySystem>(), ctx.gameTags,
        "State.Blocking", npc.blocking);
}

} // namespace game
