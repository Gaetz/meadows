// Follower CORE: shared helpers, lifecycle (recruit/dismiss/stances),
// the per-frame sweep and party commands. The combat rules live in
// FollowerCombat.cpp, the social surface in FollowerSocial.cpp — one
// TU per trade, the class stays whole in the header.
#include "game/scenes/FollowerController.hpp"

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp" // data::ActorForm, ConsumableForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp" // collectChildren (affinity rules)
#include "data/forms/LocForms.hpp"  // data::TextTable (toasts)
#include "engine/core/Log.hpp"
#include "engine/ui/UiSystem.hpp" // the recruit-preview model
#include "game/ScreenStack.hpp"   // show("recruit")
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "game/SaveGame.hpp"                        // PendingSaveLayer
#include "game/scenes/NpcDirector.hpp"
#include "game/scenes/NpcMovement.hpp" // groundAt
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp" // applyEffect (revive)
#include "gameplay/actors/ActorState.hpp" // gameplay::FollowerState
#include "gameplay/actors/FollowerForms.hpp" // FollowerClassForm,
                                             // AffinityRuleForm
#include "gameplay/actors/Followers.hpp"  // followTuning, adoptOnHit,
                                          // resolveBleedout,
                                          // affinityDelta
#include "gameplay/combat/Combat.hpp"     // updateLifeState (revive)
#include "gameplay/event/EventBus.hpp"    // gameplay::Event (aggro)
#include "gameplay/interaction/FurnitureForms.hpp" // the grave form
#include "gameplay/inventory/Inventory.hpp" // the player's bag (revive)
#include "gameplay/stats/CoreAttributes.hpp" // the 9 bases (level-ups)
#include "gameplay/stats/Damage.hpp"        // CombatState, updateDowned
#include "gameplay/stats/GameClock.hpp"     // convalescence stamps
#include "gameplay/stats/Injuries.hpp"      // Injuries
#include "world/scene/Components.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/WorldForms.hpp" // world::ReferenceForm

namespace game {


// The follower's authored ActorForm, or null when this entity is not an
// authored follower (followerCategory empty — the identity fields).
const data::ActorForm* FollowerController::followerActorForm(const FollowerContext& ctx,
                                         ecs::Entity follower) {
    if (!follower.is_alive() || !follower.has<world::RefId>() ||
        !follower.has<gameplay::FollowerState>()) {
        return nullptr;
    }
    const auto& refId = follower.get<world::RefId>();
    const data::Form* base = ctx.forms.get(refId.base);
    const reflect::TypeInfo* type = ctx.forms.typeOf(refId.base);
    if (!base || !type ||
        !type->isA(data::ActorForm::staticTypeInfo().id)) {
        return nullptr;
    }
    const auto* actor = static_cast<const data::ActorForm*>(base);
    return actor->followerCategory.empty() ? nullptr : actor;
}

// The toast line, when the scene wired one (tests run without).
void FollowerController::toast(const FollowerContext& ctx, str line) {
    if (ctx.say && !line.empty()) {
        ctx.say(std::move(line));
    }
}

// The follower's display name for toasts (his ActorForm's, like the HUD).
str FollowerController::followerDisplayName(const FollowerContext& ctx, ecs::Entity follower) {
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    return actor ? actor->displayName : str { "?" };
}

// The party census — every ACTIVE follower in the world, bucketed by
// his category (actives always live: they travel with the player).
gameplay::PartyCounts FollowerController::countActiveParty(const FollowerContext& ctx) {
    gameplay::PartyCounts counts;
    ctx.world.handle().each(
        [&](flecs::entity entity, const gameplay::FollowerState& state) {
            if (!state.followerActive) {
                return;
            }
            if (const data::ActorForm* actor =
                    followerActorForm(ctx, ecs::Entity { entity })) {
                gameplay::countPartyMember(counts, actor->followerCategory);
            }
        });
    return counts;
}

// Comments/banter never fire in sneak (docs/FOLLOWERS.md §6.1) — the
// player's State.Sneaking tag is the one signal (PlayerController syncs it).
bool FollowerController::playerSneaking(const FollowerContext& ctx) {
    if (!ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::AbilitySystem>()) {
        return false;
    }
    const auto tag = ctx.gameTags.find("State.Sneaking");
    return tag &&
           ctx.playerEntity.get<gameplay::AbilitySystem>().tags.has(*tag);
}


// ---- Classes, levels, evolution -----------------------------------------

void FollowerController::applyLevelSync(const FollowerContext& ctx,
                                        ecs::Entity follower,
                                        const data::ActorForm& actor,
                                        gameplay::FollowerState& state,
                                        bool active) {
    if (!ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::AttributeSet>()) {
        return;
    }
    const f32 playerLevel =
        ctx.playerEntity.get<gameplay::AttributeSet>().level;
    if (playerLevel == state.followerLastLevelSyncedFrom) {
        return; // already synced from this value
    }
    const gameplay::LevelSync sync = gameplay::syncFollowerLevel(
        state.followerLevel, state.followerLastLevelSyncedFrom, playerLevel,
        active, actor.mainCharacter);
    const f32 fromLevel = state.followerLevel;
    state.followerLastLevelSyncedFrom = sync.syncedFrom;
    state.followerLevel = sync.level;
    if (sync.level == fromLevel && sync.pointsGained == 0) {
        return; // stamp only (first meeting / lowered player level)
    }
    // §2.9: the sanctioned level-up base writes — the curve DELTA (so
    // earlier +1 points and instant-effect history survive), the level
    // attribute, then the +1 point walk. The follower's next
    // tickCharacter recomputes the currents with vitals PRESERVED (the
    // recomputeStats path — never initializeActorStats mid-game).
    str bonusText;
    if (follower.has<gameplay::CoreAttributes>()) {
        auto& core = follower.get_mut<gameplay::CoreAttributes>();
        if (const auto* cls = ctx.forms.find<gameplay::FollowerClassForm>(
                actor.followerClass)) {
            gameplay::applyClassLevelChange(core, *cls, fromLevel,
                                            sync.level);
        }
        // The doc §3 algorithm, v1 on ATTRIBUTES (skills come
        // later): the player's best attribute still above the
        // follower's takes the point.
        if (sync.pointsGained > 0 &&
            ctx.playerEntity.has<gameplay::CoreAttributes>()) {
            const auto& playerCore =
                ctx.playerEntity.get<gameplay::CoreAttributes>();
            for (i32 i = 0; i < sync.pointsGained; ++i) {
                const auto pick = gameplay::bonusAttribute(playerCore, core);
                if (!pick) {
                    break; // he matches the player everywhere: no point
                }
                gameplay::coreAttributeRef(core, *pick) += 1.0f;
                bonusText += str { " (+1 " } +
                             gameplay::kCoreAttributeNames[*pick] + ")";
            }
        }
    }
    if (follower.has<gameplay::AttributeSet>()) {
        follower.get_mut<gameplay::AttributeSet>().level = sync.level;
    }
    // Newly reached tiers unlock their class perks (idempotent —
    // grantAbility dedup + the grantedTag discipline skip what he has).
    if (follower.has<gameplay::AttributeSet>() &&
        follower.has<gameplay::AbilitySystem>()) {
        const i32 granted = gameplay::syncClassPerks(
            ctx.forms, actor.followerClass, sync.level,
            follower.get_mut<gameplay::AttributeSet>(),
            follower.get_mut<gameplay::AbilitySystem>(), ctx.gameTags);
        if (granted > 0) {
            LOG_INFO("{} unlocks {} class perk(s) at level {:.0f}",
                     actor.editorId, granted, sync.level);
        }
    }
    LOG_INFO("{} level {:.0f} -> {:.0f}{}", actor.editorId, fromLevel,
             sync.level, bonusText);
}

void FollowerController::recruit(const FollowerContext& ctx,
                                 ecs::Entity follower) {
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    if (!actor) {
        return; // not an authored follower (or no live reference)
    }
    auto& state = follower.get_mut<gameplay::FollowerState>();
    if (state.followerActive) {
        return;
    }
    // A convalescent follower refuses (the dialogue's refusal option
    // is the UX; this is the belt-and-braces code gate).
    if (gameplay::followerConvalescent(state, ctx.gameClock.gameHours())) {
        LOG_INFO("recruit refused — '{}' is convalescent for {:.1f} h",
                 actor->editorId,
                 state.followerDownedRecoveryHours -
                     ctx.gameClock.gameHours());
        return;
    }
    // The party caps (docs/FOLLOWERS.md §1 — 5 majors + 6 minors;
    // mounts exempt). The census and the gate are the pure doctested
    // layer; the caps are §5 tuning knobs.
    const gameplay::RecruitVerdict verdict = gameplay::canJoinParty(
        actor->followerCategory, countActiveParty(ctx),
        static_cast<i32>(ctx.statsTuning.followerMajorCap),
        static_cast<i32>(ctx.statsTuning.followerMinorCap));
    if (verdict != gameplay::RecruitVerdict::Ok) {
        if (ctx.texts) {
            toast(ctx, ctx.texts->format("follower.partyFull",
                                         actor->displayName));
        }
        LOG_INFO("recruit refused — party {} cap reached ('{}')",
                 verdict == gameplay::RecruitVerdict::MajorsFull ? "major"
                                                                 : "minor",
                 actor->editorId);
        return;
    }
    state.followerActive = true;
    // A fresh recruit always starts on the default stance (a stayed
    // then dismissed follower must not reload wedged in Stay).
    gameplay::setFollowerStance(state, gameplay::FollowerStance::Follow);
    // Mirror the protection onto HIS tags right away — 0 HP routes
    // to Downed from the very first hit (updateDowned re-syncs per frame).
    if (follower.has<gameplay::AbilitySystem>()) {
        gameplay::syncStateTag(follower.get_mut<gameplay::AbilitySystem>(),
                               ctx.gameTags, "Follower.Protected", true);
    }
    // The re-meet catch-up — half the level gap accrued apart
    // (floored), FULL for a mainCharacter (docs/FOLLOWERS.md §2); no +1
    // points for catch-up levels (those are earned traveling together).
    // Runs BEFORE captureEntity so the pending layer carries the new
    // level and attributes.
    applyLevelSync(ctx, follower, *actor, state, /*active=*/false);
    // The pending-save contract: the live entity joins the persistent set —
    // cell -> 0 like the player. captureEntity diffs the live RefId.cell
    // (now null) against the resolved ReferenceForm and writes the
    // field-level `cell = 0` patch into the pending layer; the disk save
    // flushes it as an ordinary §5 patch, and re-resolving on load spawns
    // him through the persistent pass at his saved position. Dropping
    // InCell keeps his origin cell's unload (delete_with) off him; the
    // pending layer's isRehomed veto keeps that cell's RELOAD from
    // spawning a double.
    follower.get_mut<world::RefId>().cell = data::FormHandle {};
    follower.remove<ecs::InCell>(flecs::Wildcard);
    ctx.pendingSave.captureEntity(follower, ctx.forms, ctx.gameTags);
    syncActiveTag(ctx);
    LOG_INFO("follower '{}' recruited (cell -> persistent)",
             actor->editorId);
}

void FollowerController::dismiss(const FollowerContext& ctx,
                                 ecs::Entity follower) {
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    if (!actor) {
        return;
    }
    auto& state = follower.get_mut<gameplay::FollowerState>();
    if (!state.followerActive) {
        return;
    }
    state.followerActive = false;
    // Off duty = back to the mortal rules (bandit-identical deaths).
    if (follower.has<gameplay::AbilitySystem>()) {
        gameplay::syncStateTag(follower.get_mut<gameplay::AbilitySystem>(),
                               ctx.gameTags, "Follower.Protected", false);
    }

    // Home = the authored homeMarker reference; fallback: the authored
    // spawn reference itself (both are ReferenceForms — §5 records).
    auto& refId = follower.get_mut<world::RefId>();
    const auto* authored =
        ctx.forms.find<world::ReferenceForm>(refId.referenceId);
    core::Guid homeCell;
    Vec3 homePos { 0.0f };
    const world::ReferenceForm* home =
        actor->homeMarker.isValid()
            ? ctx.forms.find<world::ReferenceForm>(actor->homeMarker)
            : nullptr;
    if (!home) {
        home = authored;
    }
    if (home) {
        homeCell = home->cell;
        homePos = home->position;
    }
    const data::FormHandle homeHandle =
        homeCell.isValid() ? ctx.forms.handleOf(homeCell) : data::FormHandle {};
    // The other half of the contract: cell -> home. Note: the streamer
    // spawns from the AUTHORED cell (the world model is built from the
    // resolved records), so a home in a DIFFERENT cell only fully lands
    // after a save/load re-resolve — Aldric's home is his authored cell.
    refId.cell = homeHandle;

    const ecs::Entity homeCellEntity =
        (ctx.cellLoader && homeHandle.isValid())
            ? ctx.cellLoader->cellEntity(homeHandle)
            : ecs::Entity {};
    if (homeCellEntity.is_alive()) {
        // Home is resident: re-parent to the cell (its unload captures and
        // deletes him like any villager) and let his schedule walk him to
        // the home marker — no teleport in the player's face.
        follower.add<ecs::InCell>(homeCellEntity);
        ctx.pendingSave.captureEntity(follower, ctx.forms, ctx.gameTags);
    } else {
        // Home not resident: park him at the marker, capture (position +
        // state land in the pending layer), despawn — he streams back in
        // with his cell, re-homed by applyReferenceOverrides +
        // finalizeActorSpawn. The caller refreshes the NPC list.
        if (follower.has<world::Transform>()) {
            auto& transform = follower.get_mut<world::Transform>();
            transform.position = homePos;
            // groundAt, not the raw height field: in interiors the terrain
            // height would park him outside the dungeon.
            groundAt(ctx.terrainParams, ctx.interiorMode, ctx.physics,
                     transform.position);
        }
        ctx.pendingSave.captureEntity(follower, ctx.forms, ctx.gameTags);
        follower.destruct();
    }
    syncActiveTag(ctx);
    LOG_INFO("follower '{}' dismissed (cell -> home, resident = {})",
             actor->editorId, homeCellEntity.is_alive());
}

void FollowerController::repositionActiveFollowers(const FollowerContext& ctx,
                                                   const Vec3& anchor) {
    const gameplay::FollowTuning tuning =
        gameplay::followTuning(ctx.statsTuning);
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        Npc& npc = *npcPtr;
        // A downed follower stays where he fell — no teleport.
        if (npc.dead || npc.downed || !npc.entity.is_alive() ||
            !npc.entity.has<gameplay::FollowerState>() ||
            !npc.entity.get<gameplay::FollowerState>().followerActive) {
            continue;
        }
        // "Stay here" means it — a stayed follower holds his spot
        // through the player's travels.
        if (gameplay::followerStance(
                npc.entity.get<gameplay::FollowerState>()) ==
            gameplay::FollowerStance::Stay) {
            continue;
        }
        Vec3 to = anchor - npc.entity.get<world::Transform>().position;
        to.y = 0.0f;
        if (glm::length(to) > tuning.teleportRadius) {
            teleportNear(anchor, ctx.terrainParams, ctx.interiorMode,
                         ctx.physics, npc);
        }
    }
}

void FollowerController::syncActiveTag(const FollowerContext& ctx) {
    // The Crime.Wanted syncTag pattern: conditions can't see components,
    // so "traveling with a follower" is mirrored as a PLAYER tag the
    // dialogue evaluator reads (HasTag Follower.Active).
    if (!ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::AbilitySystem>()) {
        return;
    }
    const gameplay::GameplayTag tag =
        ctx.gameTags.registerTag("Follower.Active");
    bool any = false;
    ctx.world.handle().each(
        [&](flecs::entity, const gameplay::FollowerState& state) {
            any = any || state.followerActive;
        });
    auto& sys = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
    if (any && !sys.tags.has(tag)) {
        sys.tags.add(tag, ctx.gameTags);
    } else if (!any && sys.tags.has(tag)) {
        sys.tags.remove(tag, ctx.gameTags);
    }
}


// ---- The per-frame follower sweep ------------------------------------

bool FollowerController::updateFollowers(const FollowerContext& ctx, f32 dt) {
    bool refreshNeeded = false;
    const auto downedTag = ctx.gameTags.find("State.Downed");
    // The passive-affinity clock — ONE stamp for the whole sweep (the
    // VendorState.lastRestockHours idiom on the shared GameClock). The
    // first frame after enter/reset only stamps.
    const f64 nowHours = ctx.gameClock.gameHours();
    const f32 deltaHours =
        lastAccrualHours_ < 0.0
            ? 0.0f
            : static_cast<f32>(nowHours - lastAccrualHours_);
    lastAccrualHours_ = nowHours;
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        Npc& npc = *npcPtr;
        if (npc.dead || !npc.entity.is_alive() ||
            !npc.entity.has<gameplay::FollowerState>() ||
            !npc.entity.has<gameplay::AbilitySystem>()) {
            continue;
        }
        auto& state = npc.entity.get_mut<gameplay::FollowerState>();
        auto& system = npc.entity.get_mut<gameplay::AbilitySystem>();
        // The routing mirror, re-synced every frame (F9 reloads drop
        // owned tags): ONLY an active follower is protected — a bandit
        // never carries the tag, so he still just dies (iso-behavior).
        gameplay::syncStateTag(system, ctx.gameTags, "Follower.Protected",
                               state.followerActive);
        // An ACTIVE, standing follower earns time together (hours +
        // affinity, clamped — gameplay::accrueTimeTogether, doctested).
        if (state.followerActive && !npc.downed) {
            gameplay::accrueTimeTogether(
                state, deltaHours, ctx.statsTuning.affinityPerHourTogether);
        }
        // An ACTIVE follower's level tracks the player's 1:1 (the
        // pure gameplay::syncFollowerLevel), each level gained granting
        // the doc's +1 attribute point. The ActorForm resolve only runs
        // on the rare frame the player's level actually moved.
        if (state.followerActive && ctx.playerEntity.is_alive() &&
            ctx.playerEntity.has<gameplay::AttributeSet>() &&
            ctx.playerEntity.get<gameplay::AttributeSet>().level !=
                state.followerLastLevelSyncedFrom) {
            if (const data::ActorForm* actor =
                    followerActorForm(ctx, npc.entity)) {
                applyLevelSync(ctx, npc.entity, *actor, state,
                               /*active=*/true);
            }
        }
        // The mercenary contract clock (the VendorState game-hour
        // idiom on the followerContractExpiryHours; the phase decision is
        // the pure gameplay::contractPhase, doctested). One warning toast
        // inside the last mercenaryWarningHours (the once-flag is runtime
        // v1 — a reload may re-warn); at expiry the dismiss
        // walks him home and the stamp clears. V1 scope: this
        // sweep sees RESIDENT followers only — an ACTIVE mercenary always
        // is (he travels with the player); a contract that lapses while
        // he is despawned resolves on next sight.
        if (state.followerActive &&
            state.followerContractExpiryHours > 0.0f && !npc.downed) {
            const data::ActorForm* actor = followerActorForm(ctx, npc.entity);
            if (actor && actor->mercenary) {
                const gameplay::ContractPhase phase = gameplay::contractPhase(
                    nowHours, state.followerContractExpiryHours,
                    ctx.statsTuning.mercenaryWarningHours);
                if (phase == gameplay::ContractPhase::Warning &&
                    !warnedContracts_[actor->id]) {
                    warnedContracts_[actor->id] = true;
                    const i32 hoursLeft = static_cast<i32>(
                        static_cast<f64>(state.followerContractExpiryHours) -
                        nowHours + 0.5);
                    if (ctx.texts) {
                        toast(ctx,
                              ctx.texts->format(
                                  "follower.contractWarning",
                                  { std::string_view { actor->displayName },
                                    std::to_string(hoursLeft) }));
                    }
                    LOG_INFO("{}'s contract ends in {} h", npc.editorId,
                             hoursLeft);
                } else if (phase == gameplay::ContractPhase::Expired) {
                    // Clear BEFORE the dismiss: its captureEntity carries
                    // the ended contract into the pending layer.
                    state.followerContractExpiryHours = 0.0f;
                    warnedContracts_.erase(actor->id);
                    if (ctx.texts) {
                        toast(ctx, ctx.texts->format("follower.contractOver",
                                                     actor->displayName));
                    }
                    LOG_INFO("{}'s contract expired — dismissed home",
                             npc.editorId);
                    ecs::Entity follower = npc.entity;
                    dismiss(ctx, follower); // may despawn npc.entity
                    refreshNeeded = true;   // the caller prunes the list
                    continue;
                }
            }
        }
        if (!npc.downed || !downedTag || !system.tags.has(*downedTag)) {
            continue;
        }
        auto& combat = npc.entity.get_mut<gameplay::CombatState>();
        if (combat.downedSeconds <= 0.0f) {
            // Odd save / missed edge: re-arm rather than hang downed.
            combat.downedSeconds = ctx.statsTuning.downedBleedoutSeconds;
        }
        if (!gameplay::updateDowned(combat, system, dt, ctx.gameTags)) {
            continue;
        }
        // Bleedout due — resolve on the seeded engine RNG (§8).
        gameplay::StatBlock block {
            npc.entity.get_mut<gameplay::CoreAttributes>(),
            npc.entity.get_mut<gameplay::AttributeSet>(), system, combat
        };
        auto& injuries = npc.entity.get_mut<gameplay::Injuries>();
        const gameplay::BleedoutResult result = gameplay::resolveBleedout(
            block, injuries, ctx.rng, ctx.gameTags, ctx.statsTuning);
        const str name = followerDisplayName(ctx, npc.entity);
        npc.downed = false;
        npc.sitting = false;
        if (result.outcome == gameplay::BleedoutOutcome::Died) {
            // Real death: the protection is lifted, State.Dead is on —
            // the director's next update mirrors npc.dead and fires the
            // normal OnDeath edge (event, cue, death anim, lootable).
            state.followerActive = false;
            syncActiveTag(ctx);
            if (ctx.texts) {
                toast(ctx, ctx.texts->format("follower.died", name));
            }
            LOG_INFO("{} bled out — real death (aggravation roll)",
                     npc.editorId);
            continue;
        }
        // Recovered at 1 HP with a fresh wound (aggravated if rolled).
        LOG_INFO("{} recovers at 1 HP with a {} wound", npc.editorId,
                 result.aggravated ? "WORSENED" : "fresh");
        if (gameplay::needsConvalescence(injuries)) {
            // Wounded past the bar: he demands rest — the dismiss
            // walks him home; the game-hour stamp gates re-recruiting.
            const f32 restHours = gameplay::convalescenceHours(injuries);
            state.followerDownedRecoveryHours =
                static_cast<f32>(ctx.gameClock.gameHours()) + restHours;
            if (ctx.texts) {
                toast(ctx, ctx.texts->format(
                              "follower.convalescence",
                              { std::string_view { name },
                                std::to_string(static_cast<i32>(
                                    restHours + 0.5f)) }));
            }
            LOG_INFO("{} needs {:.0f} h of convalescence — dismissed "
                     "home",
                     npc.editorId, restHours);
            ecs::Entity follower = npc.entity;
            dismiss(ctx, follower); // may despawn npc.entity
            refreshNeeded = true;   // the caller prunes the director list
        } else if (ctx.texts) {
            toast(ctx, ctx.texts->format("follower.revived", name));
        }
    }
    // The ambient-life tick — inter-follower banter (and its queued
    // reply line) rides the same per-frame sweep.
    updateBanter(ctx, dt, nowHours);
    syncConvalescentTag(ctx);
    return refreshNeeded;
}

// ---- Group commands, banter, ambient comments ----------------------------

void FollowerController::partyCommand(const FollowerContext& ctx,
                                      gameplay::FollowerStance stance) {
    // "Attack my target": resolve the one-shot adoption target —
    // the last hostile the player struck (the aggro signal), still standing.
    ecs::Entity attackTarget {};
    if (stance == gameplay::FollowerStance::Attack &&
        playerTarget_.is_alive()) {
        for (const auto& npcPtr : ctx.npcDirector.npcs()) {
            if (npcPtr->entity.id() == playerTarget_.id()) {
                if (npcPtr->hostile && !npcPtr->dead && !npcPtr->downed) {
                    attackTarget = playerTarget_;
                }
                break;
            }
        }
    }
    i32 commanded = 0;
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        Npc& npc = *npcPtr;
        if (npc.dead || !npc.entity.is_alive() ||
            !npc.entity.has<gameplay::FollowerState>()) {
            continue;
        }
        auto& state = npc.entity.get_mut<gameplay::FollowerState>();
        if (!state.followerActive) {
            continue;
        }
        gameplay::setFollowerStance(state, stance);
        if (stance == gameplay::FollowerStance::Stay) {
            // Hold position: stop the current walk — the follow dispatch
            // skips him from now on; his schedule stays out of it until
            // a DISMISS (staying keeps him active, standing at his spot).
            npc.path.clear();
            npc.pathIndex = 0;
            npc.speed = 0.0f;
        } else if (stance == gameplay::FollowerStance::Attack &&
                   attackTarget.is_alive() && !npc.downed &&
                   npc.entity != attackTarget) {
            // One shot at command time; afterwards the stance behaves as
            // Follow (the standing aggro table).
            npc.combatTarget = attackTarget;
        }
        // The stance travels with the save (the pending-save contract —
        // FollowerState rides the SavedStatsForm name-match sweep).
        ctx.pendingSave.captureEntity(npc.entity, ctx.forms, ctx.gameTags);
        ++commanded;
    }
    if (commanded == 0) {
        return; // no active follower heard the order
    }
    if (ctx.texts) {
        switch (stance) {
        case gameplay::FollowerStance::Follow:
            toast(ctx, ctx.texts->get("follower.partyFollow"));
            break;
        case gameplay::FollowerStance::Stay:
            toast(ctx, ctx.texts->get("follower.partyStay"));
            break;
        case gameplay::FollowerStance::Attack:
            toast(ctx, ctx.texts->get(attackTarget.is_alive()
                                          ? "follower.partyAttack"
                                          : "follower.partyNoTarget"));
            break;
        case gameplay::FollowerStance::Defend:
            toast(ctx, ctx.texts->get("follower.partyDefend"));
            break;
        }
    }
    LOG_INFO("party command stance={} ({} follower(s))",
             static_cast<i32>(stance), commanded);
}


void FollowerController::syncConvalescentTag(const FollowerContext& ctx) {
    // The Follower.Active precedent: conditions read PLAYER tags, so the
    // recruit-refusal option gates on this mirror. V1 scope: the mirror
    // is exact whenever the follower's cell is resident — which it always
    // is when you are close enough to TALK to him.
    if (!ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::AbilitySystem>()) {
        return;
    }
    const gameplay::GameplayTag tag =
        ctx.gameTags.registerTag("Follower.Convalescent");
    const f64 now = ctx.gameClock.gameHours();
    bool any = false;
    ctx.world.handle().each(
        [&](flecs::entity, const gameplay::FollowerState& state) {
            any = any || gameplay::followerConvalescent(state, now);
        });
    auto& sys = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
    if (any && !sys.tags.has(tag)) {
        sys.tags.add(tag, ctx.gameTags);
    } else if (!any && sys.tags.has(tag)) {
        sys.tags.remove(tag, ctx.gameTags);
    }
}


void FollowerController::teleportNear(const Vec3& anchor,
                                      const render::TerrainParams& terrain,
                                      bool interiorMode,
                                      phys::PhysicsWorld* physics,
                                      Npc& npc) {
    auto& transform = npc.entity.get_mut<world::Transform>();
    // A fixed side-step off the anchor (feel pass later), grounded the way
    // every NPC step is (groundAt — collision probe in interiors).
    Vec3 spot = anchor + Vec3 { 1.6f, 0.0f, -1.6f };
    spot.y = anchor.y;
    groundAt(terrain, interiorMode, physics, spot);
    transform.position = spot;
    npc.path.clear();
    npc.pathIndex = 0;
    npc.repathTimer = 0.0f;
    npc.speed = 0.0f;
}

} // namespace game
