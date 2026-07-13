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
#include "gameplay/actors/ActorState.hpp"  // FollowerState (É2 defender gate)
#include "gameplay/actors/Followers.hpp"   // pickPower, pickHealTarget (É6)
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

void NpcCombatController::tryUsePower(
    f32 dt, const NpcContext& ctx, Npc& npc,
    const std::unordered_map<u64, Npc*>& npcByEntity) {
    npc.powerRetryTimer -= dt;
    if (npc.powerRetryTimer > 0.0f) {
        return;
    }
    if (!npc.entity.has<gameplay::FollowerState>() ||
        !npc.entity.get<gameplay::FollowerState>().followerActive) {
        return;
    }
    // The power = the first granted ability that isn't the shared attack
    // (v1 — granted by the class's level-1 ClassPerkForm, persisted as
    // SavedAbilityForm rows).
    const core::Guid power = gameplay::pickPower(
        npc.entity.get<gameplay::AbilitySystem>().grantedAbilities,
        ctx.attackAbility ? ctx.attackAbility->id : core::Guid {});
    if (!power.isValid()) {
        return; // his class grants no power — nothing to retry
    }
    const auto* ability = ctx.forms.find<gameplay::AbilityForm>(power);
    if (!ability) {
        return;
    }
    // Bound the call rate; the REAL cadence gates are the ability's own
    // cost/cooldown effects — tryActivate refuses on either (§6).
    npc.powerRetryTimer = ctx.statsTuning.followerPowerRetrySeconds;
    auto& casterSet = npc.entity.get_mut<gameplay::AttributeSet>();
    auto& casterSystem = npc.entity.get_mut<gameplay::AbilitySystem>();
    const gameplay::AbilityContext abilityCtx { ctx.forms, ctx.gameTags };
    if (npc.combatStyle == "healer") {
        // The party's vitals — player, active followers, herself. The
        // downed/dead are excluded: reviving is the potion mechanic (É3),
        // not a spell target.
        // É9 NOTE — the doc's « garde toujours un objet de soin en
        // réserve pour le joueur » (docs/FOLLOWERS.md §6.2) is a stated
        // DEVIATION here: Maela heals by POWER (essence-costed ability),
        // not by items, so there is no item stock to reserve. The rule
        // becomes relevant the day a follower heals from his inventory;
        // its spirit already holds — pickHealTarget includes the player,
        // and the essence cost is the natural reserve.
        const auto fractionOf = [](const gameplay::AbilitySystem& system) {
            const f32 maxHealth = glm::max(
                gameplay::currentValueOf(system,
                                         gameplay::attr("maxHealth")),
                1.0f);
            return gameplay::currentValueOf(system,
                                            gameplay::attr("health")) /
                   maxHealth;
        };
        vector<gameplay::AllyVitals> allies;
        if (ctx.playerEntity.is_alive() &&
            ctx.playerEntity.has<gameplay::AbilitySystem>() &&
            !ctx.godMode) {
            allies.push_back(
                { ctx.playerEntity.id(),
                  fractionOf(
                      ctx.playerEntity.get<gameplay::AbilitySystem>()) });
        }
        for (const auto& [id, ally] : npcByEntity) {
            if (!ally || ally->dead || ally->downed ||
                !ally->entity.is_alive() ||
                !ally->entity.has<gameplay::FollowerState>() ||
                !ally->entity.get<gameplay::FollowerState>()
                     .followerActive) {
                continue;
            }
            allies.push_back(
                { id,
                  fractionOf(
                      ally->entity.get<gameplay::AbilitySystem>()) });
        }
        // Order-independent pick (lowest fraction, ties on id) — the
        // unordered map sweep stays deterministic (§8).
        const u64 pick = gameplay::pickHealTarget(
            allies, ctx.statsTuning.followerHealThreshold);
        if (pick == 0) {
            return; // everyone healthy: keep the essence
        }
        const ecs::Entity target = pick == ctx.playerEntity.id()
                                       ? ctx.playerEntity
                                       : npcByEntity.at(pick)->entity;
        auto& targetSet = target.get_mut<gameplay::AttributeSet>();
        auto& targetSystem = target.get_mut<gameplay::AbilitySystem>();
        if (gameplay::tryActivate(*ability, casterSet, casterSystem,
                                  targetSet, targetSystem, abilityCtx)) {
            // Instant heals write the BaseValue; re-derive the currents
            // (the reviveDownedAlly idiom).
            gameplay::recomputeCurrent(targetSet, targetSystem);
            LOG_INFO("É6: {} casts her power on {} (heal)", npc.editorId,
                     pick == ctx.playerEntity.id()
                         ? str { "the player" }
                         : npcByEntity.at(pick)->editorId);
        }
        return;
    }
    // Default / "melee": the self-buff on engaging (Aldric's war cry).
    if (gameplay::tryActivate(*ability, casterSet, casterSystem, casterSet,
                              casterSystem, abilityCtx)) {
        LOG_INFO("É6: {} unleashes his power (self-buff)", npc.editorId);
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
        // Per-faction bounty (2026-07-13): a guard hunts only when HIS
        // faction holds a slice (an old save's unattributed total counts
        // toward every faction — bountyToward folds the remainder in).
        if (wanted && ctx.playerEntity.has<gameplay::Bounty>()) {
            wanted = gameplay::bountyToward(
                         ctx.playerEntity.get<gameplay::Bounty>(),
                         npc.factionTag) > 0.0f;
        }
    }
    // É2: validate the adopted combat target — an entity may die or
    // despawn (cell unload) between frames. The aggro handler
    // (FollowerController) adopts, OnDeath clears; this is the
    // belt-and-braces sweep before any read.
    if (npc.combatTarget.id() != 0) {
        const auto it = npcByEntity.find(npc.combatTarget.id());
        const Npc* targetNpc = it != npcByEntity.end() ? it->second : nullptr;
        // É3: DOWNED reads as not-alive-for-combat — the attacker
        // disengages instead of beating a kneeling ally/enemy.
        if (!npc.combatTarget.is_alive() || !targetNpc || targetNpc->dead ||
            targetNpc->downed) {
            npc.combatTarget = ecs::Entity {};
        }
    }
    const bool entityTarget = npc.combatTarget.id() != 0 && ctx.playMode;
    if (!entityTarget &&
        !((npc.hostile || wanted) && ctx.playMode && ctx.player)) {
        return false; // the exact pre-É2 gate (iso-behavior)
    }
    bool inCombat = false;
    auto& transform = npc.entity.get_mut<world::Transform>();
    const auto& npcSys = npc.entity.get<gameplay::AbilitySystem>();
    // É2: resolve the frame's CombatTarget. The PLAYER path keeps every
    // historical read byte-for-byte (perception cone + LOS + the state
    // machine + the B3 help shout). An ENTITY target is already KNOWN —
    // it was adopted from a landed hit on the bus — so there is no
    // vision cone and no perception mutation; the LOS raycast still
    // gates strikes/approach so walls keep mattering.
    CombatTarget target;
    bool canSee = false;
    world::AwareState aware = world::AwareState::Alert;
    Vec3 lastKnownPos { 0.0f };
    if (entityTarget) {
        target.entity = npc.combatTarget;
        target.position = npc.combatTarget.get<world::Transform>().position;
        target.feet = target.position;
        target.crouched = false; // NPCs don't sneak (yet)
        target.alive = true;     // validated above
        canSee = true;
        if (ctx.physics) {
            canSee = hasLineOfSight(
                *ctx.physics,
                transform.position + Vec3 { 0.0f, 1.5f, 0.0f },
                target.position + Vec3 { 0.0f, 1.2f, 0.0f });
        }
        lastKnownPos = target.position;
    } else {
        target.entity = ctx.playerEntity;
        target.position = ctx.player->position();
        target.feet = target.position;
        target.crouched = playerSneaking;
        target.alive = ctx.playerEntity.is_alive();
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
        canSee = world::inViewCone(sight, transform.position,
                                   facing, target.position);
        if (canSee && ctx.physics) {
            canSee = hasLineOfSight(
                *ctx.physics,
                transform.position + Vec3 { 0.0f, 1.5f, 0.0f },
                target.position +
                    Vec3 { 0.0f, playerSneaking ? 0.6f : 1.2f, 0.0f });
        }
        const world::AwareState wasAware = world::awareState(perception);
        world::updatePerception(perception, canSee, target.position, dt);
        aware = world::awareState(perception);
        // B3: entering Alert shouts — a bus event for listeners, and
        // same-faction allies in earshot join the hunt.
        if (aware == world::AwareState::Alert &&
            wasAware != world::AwareState::Alert) {
            callForHelp(ctx, npc, perception.lastKnownPos, npcByEntity);
        }
        lastKnownPos = perception.lastKnownPos;
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
        // FOLLOWERS É6: an active follower tries his special power (the
        // class-perk ability) — self-buff or ally heal per combat style;
        // tryActivate's cost/cooldown effects are the real gate.
        tryUsePower(dt, ctx, npc, npcByEntity);
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
        Vec3 toTarget = target.position - transform.position;
        toTarget.y = 0.0f;
        const f32 targetDistance = glm::length(toTarget);
        // B3: the whole behavior choice is ONE sim-pure function
        // (gameplay/combat/CombatAi) — this block only executes
        // the move it returns.
        const f32 maxHealth = glm::max(
            gameplay::currentValueOf(npcSys,
                                     gameplay::attr("maxHealth")),
            1.0f);
        const gameplay::CombatSituation situation {
            targetDistance,
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
        // FOLLOWERS É6: a "healer" holds a SUPPORT band instead of
        // closing in — Strike/Approach re-route onto the existing
        // strafe/flee machinery around healerPreferredDistance (the
        // archer-band idea; docs/FOLLOWERS.md §7 tactical roles). Only
        // for an ACTIVE follower — a hostile with the style keeps the
        // default brain (iso-behavior).
        f32 strafeRange = attackRange;
        f32 strafeReach = reach;
        if (npc.combatStyle == "healer" &&
            npc.entity.has<gameplay::FollowerState>() &&
            npc.entity.get<gameplay::FollowerState>().followerActive) {
            const f32 preferred =
                glm::max(ctx.statsTuning.healerPreferredDistance, 2.0f);
            strafeRange = preferred - 0.5f;
            strafeReach = preferred - 0.5f;
            if (move == gameplay::CombatMove::Strike ||
                move == gameplay::CombatMove::Approach) {
                move = targetDistance < preferred * 0.75f
                           ? gameplay::CombatMove::Flee
                           : gameplay::CombatMove::Strafe;
            }
        }
        // Intent trace (dev report 2026-07-12: a bow-band strafe reads
        // as a flee from the outside) — one line per TRANSITION, with
        // the health fraction so a real flee is self-explaining.
        if (npc.combatMove != move) {
            npc.combatMove = move;
            npc.intentReason =
                str { "combat: " } + gameplay::combatMoveName(move);
            LOG_INFO("B3: {} -> {} (health {:.0f}%, dist {:.1f} m)",
                     npc.editorId, gameplay::combatMoveName(move),
                     situation.healthFraction * 100.0f, targetDistance);
        }
        switch (move) {
        case gameplay::CombatMove::Strike:
            strike(ctx, npc, transform, npcWeapon, swinging, quiverDry,
                   toTarget, target);
            break;
        case gameplay::CombatMove::Strafe:
            strafe(dt, ctx, npc, transform, toTarget, targetDistance,
                   strafeRange, strafeReach); // É6: the healer's wide band
            break;
        case gameplay::CombatMove::Flee:
            flee(dt, ctx, npc, transform, toTarget, targetDistance);
            break;
        case gameplay::CombatMove::Approach:
            approach(dt, ctx, npc, transform, canSee, swinging,
                     target.position, lastKnownPos, aware);
            break;
        }
    }
    return inCombat;
}

void NpcCombatController::strike(const NpcContext& ctx, Npc& npc,
                                 world::Transform& transform,
                                 const data::WeaponForm* npcWeapon,
                                 bool swinging, bool quiverDry,
                                 const Vec3& toTarget,
                                 const CombatTarget& target) {
    npc.path.clear();
    // Face the target and swing.
    npc.yaw = std::atan2(toTarget.x, toTarget.z);
    transform.rotation = glm::angleAxis(
        npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    // É2: god mode shields the PLAYER only — an adopted entity target
    // has no such armor (identical to the pre-É2 line when the target
    // is the player: target.alive == playerEntity.is_alive()).
    const bool shielded =
        target.entity == ctx.playerEntity && ctx.godMode;
    // P0 A3: instant damage became an ability-gated
    // MeleeSwing — the Sword_Attack clip carries the
    // hand, and the blade must TOUCH (updateSwing).
    if (!swinging && npc.attackCooldown <= 0.0f &&
        npcWeapon && target.alive && !shielded) {
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
            fireArrow(ctx, npc, transform, *npcWeapon, target.position);
        } else if (activated) {
            gameplay::startSwing(
                npc.entity
                    .get_mut<gameplay::MeleeSwing>());
            // Pause between swings: the weapon's field, or
            // the melee fallback when unset (R7).
            npc.attackCooldown =
                npcWeapon->attackCooldown > 0.0f
                    ? npcWeapon->attackCooldown
                    : 1.6f;
            npc.blocking = false; // guard drops to strike
        }
    }
}

void NpcCombatController::fireArrow(const NpcContext& ctx, Npc& npc,
                                    world::Transform& transform,
                                    const data::WeaponForm& npcWeapon,
                                    const Vec3& targetPos) {
    // A7: an ARCHER — loose from the chest at
    // the target's chest (player or É2 entity
    // target alike), with a hair of spread
    // (deterministic combat RNG, §8).
    gameplay::Projectile arrow;
    arrow.position = transform.position +
                     Vec3 { 0.0f, 1.4f, 0.0f };
    Vec3 aim =
        (targetPos + Vec3 { 0.0f, 1.0f, 0.0f }) -
        arrow.position;
    aim = glm::normalize(aim);
    aim.x += (static_cast<f32>(
                  ctx.combatRng.unit()) -
              0.5f) *
             ctx.statsTuning.archerSpread;
    aim.z += (static_cast<f32>(
                  ctx.combatRng.unit()) -
              0.5f) *
             ctx.statsTuning.archerSpread;
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
    // Pause between shots: the weapon's field, or the ranged fallback (R7).
    npc.attackCooldown = npcWeapon.attackCooldown > 0.0f
                             ? npcWeapon.attackCooldown
                             : 2.2f;
    npc.blocking = false;
}

void NpcCombatController::strafe(f32 dt, const NpcContext& ctx, Npc& npc,
                                 world::Transform& transform,
                                 const Vec3& toTarget, f32 targetDistance,
                                 f32 attackRange, f32 reach) {
    npc.path.clear();
    // Orbit the target: a tangent direction whose side is
    // picked by entity-id parity (stable per NPC — two
    // bandits circle opposite ways) plus a radial drift
    // holding the middle of the weapon band.
    const Vec3 radial =
        toTarget / glm::max(targetDistance, 1e-3f);
    const f32 side =
        (npc.entity.id() & 1u) != 0u ? 1.0f : -1.0f;
    const f32 band = (attackRange + reach + 1.0f) * 0.5f;
    Vec3 dir =
        glm::cross(Vec3 { 0.0f, 1.0f, 0.0f }, radial) *
            side +
        radial * glm::clamp(targetDistance - band, -1.0f,
                            1.0f) *
            0.6f;
    dir.y = 0.0f;
    const f32 len = glm::length(dir);
    // Wall guard: direct steering skips the navigator,
    // so probe the step — blocked = hold ground (still
    // facing the target).
    if (len > 1e-4f &&
        !steerBlocked(ctx, transform.position,
                      dir / len)) {
        moveNpcDirect(ctx, npc, dt, dir / len, 1.0f,
                      std::atan2(toTarget.x, toTarget.z));
    }
}

void NpcCombatController::flee(f32 dt, const NpcContext& ctx, Npc& npc,
                               world::Transform& transform,
                               const Vec3& toTarget, f32 targetDistance) {
    // Flee through the NAVIGATOR (dev report 2026-07-12:
    // direct steering ran straight through buildings) —
    // path to a spot away from the target, repathed as
    // the flight goes on; obstacles are its job.
    const Vec3 away =
        targetDistance > 1e-3f
            ? -toTarget / targetDistance
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
                                   bool swinging, const Vec3& targetPos,
                                   const Vec3& lastKnownPos,
                                   world::AwareState aware) {
    // Hunt the target while seen; investigate the last
    // known position otherwise (B2).
    const Vec3 goal =
        canSee ? targetPos : lastKnownPos;
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

void NpcCombatController::updateSwing(
    f32 dt, const NpcContext& ctx, Npc& npc,
    const data::WeaponForm* npcWeapon, bool playerSneaking,
    const std::unordered_map<u64, Npc*>& npcByEntity) {
    // P0 A3/A4: the swing machine + the blade-touch hit (the SAME
    // MeleeSwing code path as the player). The clip's authored
    // HitOpen/HitClose events override the data windows; the hit
    // segment is the VISIBLE blade — world x hand joint x +Y, exactly
    // what extract() draws — against the DEFENDER's capsule. É2: the
    // defender is the combat target — the player by default (every
    // pre-É2 read intact: godMode gate, sneak capsule, dodge i-frames),
    // the adopted entity otherwise (godMode and dodging are PLAYER
    // concepts; NPCs don't dodge yet).
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
        const bool entityTarget =
            npc.combatTarget.id() != 0 && npc.combatTarget.is_alive();
        const ecs::Entity defenderEntity =
            entityTarget ? npc.combatTarget : ctx.playerEntity;
        const Npc* defenderNpc = nullptr;
        if (entityTarget) {
            const auto it = npcByEntity.find(defenderEntity.id());
            defenderNpc = it != npcByEntity.end() ? it->second : nullptr;
        }
        // An ACTIVE FOLLOWER only ever hits its adopted target — if the
        // target died mid-swing (OnDeath cleared it), the rest of the
        // Active window must NOT fall back onto the player's capsule.
        const bool followerActive =
            npc.entity.has<gameplay::FollowerState>() &&
            npc.entity.get<gameplay::FollowerState>().followerActive;
        const bool defenderValid =
            entityTarget ? (defenderNpc && !defenderNpc->dead &&
                            !defenderNpc->downed) // É3: blades skip the downed
                         : (!followerActive && ctx.player &&
                            ctx.playerEntity.is_alive() && !ctx.godMode);
        if (swing.phase == gameplay::SwingPhase::Active &&
            npc.handJoint >= 0 && ctx.playMode && defenderValid) {
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
            const Vec3 feet =
                entityTarget
                    ? defenderEntity.get<world::Transform>().position
                    : ctx.player->position();
            // Dodge i-frames: State.Dodging means the blade passes
            // through — and does NOT register, so the same Active
            // window can still connect once the i-frames expire.
            // (Player only: NPCs have no dodge yet.)
            bool dodging = false;
            if (!entityTarget) {
                if (const auto dodgeTag =
                        ctx.gameTags.find("State.Dodging")) {
                    dodging = ctx.playerEntity
                                  .get<gameplay::AbilitySystem>()
                                  .tags.has(*dodgeTag);
                }
            }
            // A crouched player is half the target (sneak rule).
            const bool crouched = !entityTarget && playerSneaking;
            if (!dodging &&
                gameplay::segmentHitsActor(grip, tip, feet, crouched) &&
                gameplay::registerStrike(swing, defenderEntity.id())) {
                // The exchange rules (crit window, guard cone,
                // perfect parry, events, cues) live in ONE place,
                // resolveMeleeStrike, shared with the player side —
                // and target-agnostic since R1 (É2 leans on that).
                gameplay::StatBlock defender {
                    defenderEntity.get_mut<gameplay::CoreAttributes>(),
                    defenderEntity.get_mut<gameplay::AttributeSet>(),
                    defenderEntity.get_mut<gameplay::AbilitySystem>(),
                    defenderEntity.get_mut<gameplay::CombatState>()
                };
                gameplay::StatBlock attacker {
                    npc.entity.get_mut<gameplay::CoreAttributes>(),
                    npc.entity.get_mut<gameplay::AttributeSet>(),
                    npc.entity.get_mut<gameplay::AbilitySystem>(),
                    npc.entity.get_mut<gameplay::CombatState>()
                };
                const auto& defenderT =
                    defenderEntity.get<world::Transform>();
                const gameplay::StrikeGeometry geo {
                    transform.position, defenderT.position,
                    defenderT.rotation * Vec3 { 0.0f, 0.0f, 1.0f },
                    defenderEntity.get<gameplay::MeleeSwing>()
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
                        defenderEntity,
                        gameplay::weaponDamageEvent(*npcWeapon,
                                                    attacker.system),
                        geo, strikeCtx);
                const str defenderName =
                    entityTarget ? defenderNpc->editorId : str { "you" };
                if (outcome.guard.perfect) {
                    LOG_INFO("PERFECT PARRY by {} — {}'s poise -{}{}",
                             defenderName, npc.editorId,
                             ctx.statsTuning.perfectParryPosture,
                             outcome.riposte.staggered
                                 ? " (STAGGERED!)" : "");
                } else {
                    LOG_INFO("{}'s blade lands on {}: {:.0f} damage{}{}",
                             npc.editorId, defenderName,
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
