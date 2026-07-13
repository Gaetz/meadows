#include "game/scenes/FollowerController.hpp"

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp" // data::ActorForm, ConsumableForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp" // collectChildren (É4 affinity rules)
#include "data/forms/LocForms.hpp"  // data::TextTable (É3 toasts)
#include "engine/core/Log.hpp"
#include "engine/ui/UiSystem.hpp" // the recruit-preview model (É4)
#include "game/ScreenStack.hpp"   // show("recruit") (É4)
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "game/SaveGame.hpp"                        // PendingSaveLayer
#include "game/scenes/NpcDirector.hpp"              // Npc, NpcDirector
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp" // applyEffect (É3 revive)
#include "gameplay/actors/ActorState.hpp" // gameplay::FollowerState
#include "gameplay/actors/FollowerForms.hpp" // FollowerClassForm,
                                             //   AffinityRuleForm (É4)
#include "gameplay/actors/Followers.hpp"  // followTuning, adoptOnHit (É2),
                                          //   resolveBleedout (É3),
                                          //   affinityDelta (É4)
#include "gameplay/combat/Combat.hpp"     // updateLifeState (É3 revive)
#include "gameplay/event/EventBus.hpp"    // gameplay::Event (É2 aggro)
#include "gameplay/interaction/FurnitureForms.hpp" // the grave form (É8)
#include "gameplay/inventory/Inventory.hpp" // the player's bag (É3 revive)
#include "gameplay/stats/CoreAttributes.hpp" // the 9 bases (É5 level-ups)
#include "gameplay/stats/Damage.hpp"        // CombatState, updateDowned (É3)
#include "gameplay/stats/GameClock.hpp"     // convalescence stamps (É3)
#include "gameplay/stats/Injuries.hpp"      // Injuries (É3)
#include "world/scene/Components.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/WorldForms.hpp" // world::ReferenceForm

namespace game {

namespace {

// The follower's authored ActorForm, or null when this entity is not an
// authored follower (followerCategory empty — É0's identity fields).
const data::ActorForm* followerActorForm(const FollowerContext& ctx,
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
void toast(const FollowerContext& ctx, str line) {
    if (ctx.say && !line.empty()) {
        ctx.say(std::move(line));
    }
}

// The follower's display name for toasts (his ActorForm's, like the HUD).
str followerDisplayName(const FollowerContext& ctx, ecs::Entity follower) {
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    return actor ? actor->displayName : str { "?" };
}

} // namespace

// ---- É5: classes, levels, evolution -----------------------------------------

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
        // The doc §3 algorithm, v1 on ATTRIBUTES (skills are their own
        // chantier): the player's best attribute still above the
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
    // É6: newly reached tiers unlock their class perks (idempotent —
    // grantAbility dedup + the grantedTag discipline skip what he has).
    if (follower.has<gameplay::AttributeSet>() &&
        follower.has<gameplay::AbilitySystem>()) {
        const i32 granted = gameplay::syncClassPerks(
            ctx.forms, actor.followerClass, sync.level,
            follower.get_mut<gameplay::AttributeSet>(),
            follower.get_mut<gameplay::AbilitySystem>(), ctx.gameTags);
        if (granted > 0) {
            LOG_INFO("É6: {} unlocks {} class perk(s) at level {:.0f}",
                     actor.editorId, granted, sync.level);
        }
    }
    LOG_INFO("É5: {} level {:.0f} -> {:.0f}{}", actor.editorId, fromLevel,
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
    // É3: a convalescent follower refuses (the dialogue's refusal option
    // is the UX; this is the belt-and-braces code gate).
    if (gameplay::followerConvalescent(state, ctx.gameClock.gameHours())) {
        LOG_INFO("É3: recruit refused — '{}' is convalescent for {:.1f} h",
                 actor->editorId,
                 state.followerDownedRecoveryHours -
                     ctx.gameClock.gameHours());
        return;
    }
    state.followerActive = true;
    // É3: mirror the protection onto HIS tags right away — 0 HP routes
    // to Downed from the very first hit (updateDowned re-syncs per frame).
    if (follower.has<gameplay::AbilitySystem>()) {
        gameplay::syncStateTag(follower.get_mut<gameplay::AbilitySystem>(),
                               ctx.gameTags, "Follower.Protected", true);
    }
    // É5: the re-meet catch-up — half the level gap accrued apart
    // (floored), FULL for a mainCharacter (docs/FOLLOWERS.md §2); no +1
    // points for catch-up levels (those are earned traveling together).
    // Runs BEFORE captureEntity so the pending layer carries the new
    // level and attributes.
    applyLevelSync(ctx, follower, *actor, state, /*active=*/false);
    // The chantier-5 contract: the live entity joins the persistent set —
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
    LOG_INFO("É1: follower '{}' recruited (cell -> persistent)",
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
    // É3: off duty = back to the mortal rules (bandit-identical deaths).
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
    // The other half of the contract: cell -> home. É1 note: the streamer
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
            transform.position.y = render::terrain::height(
                ctx.terrainParams, homePos.x, homePos.z);
        }
        ctx.pendingSave.captureEntity(follower, ctx.forms, ctx.gameTags);
        follower.destruct();
    }
    syncActiveTag(ctx);
    LOG_INFO("É1: follower '{}' dismissed (cell -> home, resident = {})",
             actor->editorId, homeCellEntity.is_alive());
}

void FollowerController::repositionActiveFollowers(const FollowerContext& ctx,
                                                   const Vec3& anchor) {
    const gameplay::FollowTuning tuning =
        gameplay::followTuning(ctx.statsTuning);
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        Npc& npc = *npcPtr;
        // É3: a downed follower stays where he fell — no teleport.
        if (npc.dead || npc.downed || !npc.entity.is_alive() ||
            !npc.entity.has<gameplay::FollowerState>() ||
            !npc.entity.get<gameplay::FollowerState>().followerActive) {
            continue;
        }
        Vec3 to = anchor - npc.entity.get<world::Transform>().position;
        to.y = 0.0f;
        if (glm::length(to) > tuning.teleportRadius) {
            teleportNear(anchor, ctx.terrainParams, npc);
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

void FollowerController::onHitTaken(const FollowerContext& ctx,
                                    const gameplay::Event& event) {
    const u64 source = event.source.id();
    const u64 target = event.target.id();
    if (source == 0 || target == 0 || source == target) {
        return; // sourceless hits (despawned shooter) drive no aggro
    }
    const auto isActiveFollower = [](const Npc& npc) {
        return npc.entity.is_alive() &&
               npc.entity.has<gameplay::FollowerState>() &&
               npc.entity.get<gameplay::FollowerState>().followerActive;
    };
    // Resolve the two parties' roles ONCE (a plain sweep of the director
    // list — hits are rare events, no index needed).
    const Npc* sourceNpc = nullptr;
    const Npc* targetNpc = nullptr;
    for (const auto& npcPtr : ctx.npcDirector.npcs()) {
        if (npcPtr->entity.id() == source) {
            sourceNpc = npcPtr.get();
        }
        if (npcPtr->entity.id() == target) {
            targetNpc = npcPtr.get();
        }
    }
    gameplay::AggroRoles roles;
    roles.sourcePlayer = event.source == ctx.playerEntity;
    roles.sourceFollower = sourceNpc && isActiveFollower(*sourceNpc);
    roles.sourceHostile = sourceNpc && sourceNpc->hostile;
    roles.targetPlayer = event.target == ctx.playerEntity;
    roles.targetFollower = targetNpc && isActiveFollower(*targetNpc);
    roles.targetHostile = targetNpc && targetNpc->hostile;
    // Friendly trial (the doc's brawl case): the PLAYER tag suppresses
    // follower adoption — the gate only, no content sets it yet.
    if (ctx.playerEntity.is_alive() &&
        ctx.playerEntity.has<gameplay::AbilitySystem>()) {
        if (const auto tag = ctx.gameTags.find("Combat.FriendlyTrial")) {
            roles.friendlyTrial =
                ctx.playerEntity.get<gameplay::AbilitySystem>().tags.has(
                    *tag);
        }
    }
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        Npc& npc = *npcPtr;
        // É3: the downed adopt nothing — they are out of the fight.
        if (npc.dead || npc.downed || !npc.entity.is_alive()) {
            continue;
        }
        roles.self = npc.entity.id();
        roles.selfFollower = isActiveFollower(npc);
        roles.selfHostile = npc.hostile;
        // "Live target" = the adopted entity still stands (alive AND its
        // director record — if it has one — isn't dead or downed yet).
        bool liveTarget = false;
        if (npc.combatTarget.id() != 0 && npc.combatTarget.is_alive()) {
            liveTarget = true;
            for (const auto& other : ctx.npcDirector.npcs()) {
                if (other->entity.id() == npc.combatTarget.id()) {
                    liveTarget = !other->dead && !other->downed;
                    break;
                }
            }
        }
        roles.selfHasLiveTarget = liveTarget;
        const u64 adopt = gameplay::adoptOnHit(source, target, roles);
        if (adopt == 0 || adopt == npc.combatTarget.id()) {
            continue;
        }
        // adoptOnHit only ever returns an NPC entity (never the player).
        const Npc* adopted = adopt == source ? sourceNpc : targetNpc;
        npc.combatTarget = adopt == source ? event.source : event.target;
        LOG_INFO("É2: {} engages {} (aggro on hit)", npc.editorId,
                 adopted ? adopted->editorId : str { "?" });
    }
}

void FollowerController::onDeath(const FollowerContext& ctx,
                                 const gameplay::Event& event) {
    const u64 dead = event.target.id();
    if (dead == 0) {
        return;
    }
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        if (gameplay::disengageOnDeath(dead, npcPtr->combatTarget.id())) {
            npcPtr->combatTarget = ecs::Entity {};
        }
    }
}

// ---- É3/É4: the per-frame follower sweep ------------------------------------

bool FollowerController::updateFollowers(const FollowerContext& ctx, f32 dt) {
    bool refreshNeeded = false;
    const auto downedTag = ctx.gameTags.find("State.Downed");
    // É4: the passive-affinity clock — ONE stamp for the whole sweep (the
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
        // É4: an ACTIVE, standing follower earns time together (hours +
        // affinity, clamped — gameplay::accrueTimeTogether, doctested).
        if (state.followerActive && !npc.downed) {
            gameplay::accrueTimeTogether(
                state, deltaHours, ctx.statsTuning.affinityPerHourTogether);
        }
        // É5: an ACTIVE follower's level tracks the player's 1:1 (the
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
            LOG_INFO("É3: {} bled out — real death (aggravation roll)",
                     npc.editorId);
            continue;
        }
        // Recovered at 1 HP with a fresh wound (aggravated if rolled).
        LOG_INFO("É3: {} recovers at 1 HP with a {} wound", npc.editorId,
                 result.aggravated ? "WORSENED" : "fresh");
        if (gameplay::needsConvalescence(injuries)) {
            // Wounded past the bar: he demands rest — the É1 dismiss
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
            LOG_INFO("É3: {} needs {:.0f} h of convalescence — dismissed "
                     "home",
                     npc.editorId, restHours);
            ecs::Entity follower = npc.entity;
            dismiss(ctx, follower); // may despawn npc.entity
            refreshNeeded = true;   // the caller prunes the director list
        } else if (ctx.texts) {
            toast(ctx, ctx.texts->format("follower.revived", name));
        }
    }
    syncConvalescentTag(ctx);
    return refreshNeeded;
}

void FollowerController::reviveDownedAlly(const FollowerContext& ctx,
                                          ecs::Entity follower) {
    if (!follower.is_alive() || !follower.has<gameplay::AbilitySystem>() ||
        !follower.has<gameplay::AttributeSet>()) {
        return;
    }
    auto& system = follower.get_mut<gameplay::AbilitySystem>();
    const auto downedTag = ctx.gameTags.find("State.Downed");
    if (!downedTag || !system.tags.has(*downedTag)) {
        return; // he already stood up (or was never down)
    }
    // The first health-restoring consumable in the PLAYER's bag — the
    // UiRouter::useConsumable identification (a ConsumableForm whose
    // EffectForm adds health), re-aimed at the follower.
    const data::ConsumableForm* potion = nullptr;
    const gameplay::EffectForm* heal = nullptr;
    if (ctx.playerEntity.is_alive() &&
        ctx.playerEntity.has<gameplay::Inventory>()) {
        for (const gameplay::ItemStack& stack :
             ctx.playerEntity.get<gameplay::Inventory>().items) {
            if (stack.count <= 0) {
                continue;
            }
            const auto* consumable =
                ctx.forms.find<data::ConsumableForm>(stack.item);
            if (!consumable || !consumable->effect.isValid()) {
                continue;
            }
            const auto* effect =
                ctx.forms.find<gameplay::EffectForm>(consumable->effect);
            if (effect && effect->attribute == "health" &&
                effect->op == "add" && effect->magnitude > 0.0f) {
                potion = consumable;
                heal = effect;
                break;
            }
        }
    }
    if (!potion) {
        if (ctx.texts) {
            toast(ctx, ctx.texts->get("follower.noPotion"));
        }
        return;
    }
    gameplay::removeItem(ctx.playerEntity.get_mut<gameplay::Inventory>(),
                         potion->id, 1);
    // §2.9: the heal is the consumable's OWN GameplayEffect, applied to
    // the FOLLOWER's stats — partial restoration is the potion's call.
    gameplay::applyEffect(follower.get_mut<gameplay::AttributeSet>(), system,
                          *heal, ctx.gameTags);
    gameplay::recomputeCurrent(follower.get<gameplay::AttributeSet>(),
                               system);
    gameplay::updateLifeState(system, ctx.gameTags); // drops State.Downed
    if (follower.has<gameplay::CombatState>()) {
        follower.get_mut<gameplay::CombatState>().downedSeconds = 0.0f;
    }
    if (ctx.texts) {
        toast(ctx, ctx.texts->format("follower.revived",
                                     followerDisplayName(ctx, follower)));
    }
    LOG_INFO("É3: revived downed ally with '{}'", potion->editorId);
}

void FollowerController::consultFollower(const FollowerContext& ctx,
                                         ecs::Entity follower) {
    if (!follower.is_alive() || !follower.has<gameplay::AbilitySystem>() ||
        !ctx.texts) {
        return;
    }
    const auto& system = follower.get<gameplay::AbilitySystem>();
    const f32 health =
        gameplay::currentValueOf(system, gameplay::attr("health"));
    const f32 maxHealth = glm::max(
        gameplay::currentValueOf(system, gameplay::attr("maxHealth")), 1.0f);
    const i32 pct = static_cast<i32>(
        glm::clamp(100.0f * health / maxHealth, 0.0f, 100.0f));
    u64 injuryCount = 0;
    f32 restHours = 0.0f;
    if (follower.has<gameplay::Injuries>()) {
        const auto& injuries = follower.get<gameplay::Injuries>();
        injuryCount = injuries.list.size();
        restHours = gameplay::convalescenceHours(injuries);
    }
    toast(ctx, ctx.texts->format(
                   "follower.status",
                   { std::string_view {
                         followerDisplayName(ctx, follower) },
                     std::to_string(pct), std::to_string(injuryCount),
                     std::to_string(
                         static_cast<i32>(restHours + 0.5f)) }));
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

// ---- É4: affinity rules + the recruit preview -------------------------------

void FollowerController::onAffinityEvent(const FollowerContext& ctx,
                                         const gameplay::Event& event,
                                         ecs::Entity dialoguePartner) {
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        Npc& npc = *npcPtr;
        if (npc.dead || !npc.entity.is_alive() ||
            !npc.entity.has<gameplay::FollowerState>()) {
            continue;
        }
        auto& state = npc.entity.get_mut<gameplay::FollowerState>();
        // Eligible: traveling together — or the one being TALKED to (his
        // chat rules must run before recruitment, or affinity-gated
        // recruiting could never unlock).
        if (!state.followerActive && npc.entity != dialoguePartner) {
            continue;
        }
        const data::ActorForm* actor = followerActorForm(ctx, npc.entity);
        if (!actor) {
            continue;
        }
        // His ActorForm's rules (the childrenOf pattern); most actors have
        // none — the sweep stays cheap on this early exit.
        const vector<const gameplay::AffinityRuleForm*> rules =
            data::collectChildren<gameplay::AffinityRuleForm>(ctx.forms,
                                                              actor->id);
        if (rules.empty()) {
            continue;
        }
        gameplay::AffinityEventView view;
        view.kind = event.kind;
        view.tag = event.tag;
        view.sourceIsPlayer = event.source == ctx.playerEntity;
        view.targetIsSelf = event.target == npc.entity;
        const f32 delta = gameplay::affinityDelta(rules, view, ctx.gameTags);
        if (delta == 0.0f) {
            continue;
        }
        const f32 applied = gameplay::addAffinity(state, delta);
        if (applied != 0.0f) {
            LOG_INFO("É4: {} affinity {:+.1f} -> {:.1f}", npc.editorId,
                     applied, state.followerAffinity);
        }
    }
}

void FollowerController::openRecruitPreview(const FollowerContext& ctx,
                                            ecs::Entity follower) {
    if (!ctx.ui || !ctx.screenStack || !ctx.texts ||
        !follower.is_alive() || !follower.has<gameplay::AbilitySystem>() ||
        !follower.has<gameplay::FollowerState>()) {
        return;
    }
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    if (!actor) {
        return;
    }
    const auto& system = follower.get<gameplay::AbilitySystem>();
    const auto& state = follower.get<gameplay::FollowerState>();
    const auto current = [&](const char* name) {
        return gameplay::currentValueOf(system, gameplay::attr(name));
    };
    const auto whole = [](f32 value) {
        return std::to_string(static_cast<i32>(value + 0.5f));
    };

    ::ui::UiSystem& ui = *ctx.ui;
    ui.setString("recruit", "name", actor->displayName);
    const auto* cls =
        ctx.forms.find<gameplay::FollowerClassForm>(actor->followerClass);
    ui.setString("recruit", "classText",
                 ctx.texts->format("ui.recruit.class",
                                   cls ? cls->displayName : str { "-" }));
    ui.setString("recruit", "levelText",
                 ctx.texts->format("ui.recruit.level",
                                   whole(current("level"))));
    ui.setString("recruit", "affinityText",
                 ctx.texts->format("ui.recruit.affinity",
                                   whole(state.followerAffinity)));
    const auto vital = [&](const char* slot, const char* key,
                           const char* attrName, const char* maxName) {
        ui.setString("recruit", slot,
                     ctx.texts->format(key, { whole(current(attrName)),
                                              whole(current(maxName)) }));
    };
    vital("healthText", "ui.recruit.health", "health", "maxHealth");
    vital("energyText", "ui.recruit.energy", "energy", "maxEnergy");
    vital("essenceText", "ui.recruit.essence", "essence", "maxEssence");

    // The nine attributes (docs/STATS.md §1), one row each — CURRENT
    // values on the partner entity, labels from the loc table (C9.5).
    static constexpr const char* kAttributes[] = {
        "strength",   "constitution", "grace", "dexterity", "alacrity",
        "perception", "charisma",     "ego",   "insight",
    };
    vector<::ui::UiRow> rows;
    rows.reserve(9);
    for (const char* name : kAttributes) {
        ::ui::UiRow row;
        row.id = name;
        row.c0 = ctx.texts->get(str { "ui.attr." } + name);
        row.c1 = whole(current(name));
        rows.push_back(std::move(row));
    }
    ui.setRows("recruit", std::move(rows));
    ctx.screenStack->show("recruit");
}

// ---- É6: the player learns a perk (réciproque) -------------------------------

void FollowerController::teachPerk(const FollowerContext& ctx,
                                   ecs::Entity follower) {
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    if (!actor || !ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::AttributeSet>() ||
        !ctx.playerEntity.has<gameplay::AbilitySystem>()) {
        return;
    }
    // His teachable perks (TaughtPerkForm children — childrenOf order =
    // plugin order, deterministic §8): the first UNLEARNED one lands.
    const vector<const gameplay::TaughtPerkForm*> perks =
        data::collectChildren<gameplay::TaughtPerkForm>(ctx.forms,
                                                        actor->id);
    auto& set = ctx.playerEntity.get_mut<gameplay::AttributeSet>();
    auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
    for (const gameplay::TaughtPerkForm* perk : perks) {
        if (gameplay::grantPerk(ctx.forms, perk->ability, perk->effect, set,
                                system,
                                ctx.gameTags) != gameplay::PerkGrant::Granted) {
            continue; // already known (or a skipped data bug): next one
        }
        if (ctx.texts) {
            toast(ctx, ctx.texts->format("follower.perkLearned",
                                         perk->displayName));
        }
        LOG_INFO("É6: the player learns '{}' from {}", perk->displayName,
                 actor->editorId);
        return;
    }
    // Nothing new to teach: say so (the option stays visible — v1).
    if (ctx.texts) {
        toast(ctx, ctx.texts->format("follower.perkNone",
                                     actor->displayName));
    }
}

// ---- É7: the forge upgrade of the base kit -----------------------------------

void FollowerController::forgeUpgrade(const FollowerContext& ctx,
                                      ecs::Entity follower) {
    // [cpp-tuning] the forge price — mirrored by the option's HasItem
    // gate in data (the payFine 40-gold precedent). Data TODO (v1 scope):
    // per-follower obsolescence tiers (level paliers) as child records,
    // and the follower/player-BOTH-at-the-forge check (v1 gates on the
    // player's Zone.Forge tag — the follower walks with him).
    constexpr i32 kForgeCost = 50;
    const data::ActorForm* actor = followerActorForm(ctx, follower);
    if (!actor || !ctx.playerEntity.is_alive() || !ctx.goldForm ||
        !follower.has<gameplay::Inventory>() ||
        !ctx.playerEntity.has<gameplay::Inventory>()) {
        return;
    }
    auto& items = follower.get_mut<gameplay::Inventory>();
    // The swap list first (§2.2: the upgrade IS the next-tier Form —
    // ArmeDAldric -> ArmeDAldricPlus in data): every unremovable weapon
    // that names a valid upgradesTo tier.
    struct Swap {
        core::Guid from, to;
        i32 count;
    };
    vector<Swap> swaps;
    for (const gameplay::ItemStack& stack : items.items) {
        if (stack.count <= 0) {
            continue;
        }
        const auto* weapon = ctx.forms.find<data::WeaponForm>(stack.item);
        if (weapon && weapon->unremovable && weapon->upgradesTo.isValid() &&
            ctx.forms.find<data::WeaponForm>(weapon->upgradesTo)) {
            swaps.push_back({ stack.item, weapon->upgradesTo, stack.count });
        }
    }
    if (swaps.empty()) {
        // Already at the top tier: refuse WITHOUT charging (why the gold
        // moves here and not through the node's takeItem).
        if (ctx.texts) {
            toast(ctx, ctx.texts->format("follower.forgeNothing",
                                         actor->displayName));
        }
        return;
    }
    // Charge (the payFine idiom — the option's HasItem >= 50 gate makes a
    // failure here a modded-data breakage: then do nothing).
    auto& bag = ctx.playerEntity.get_mut<gameplay::Inventory>();
    if (!gameplay::removeItem(bag, ctx.goldForm->id, kForgeCost)) {
        return;
    }
    for (const Swap& swap : swaps) {
        gameplay::removeItem(items, swap.from, swap.count);
        gameplay::addItem(items, swap.to, swap.count);
        if (follower.has<gameplay::Equipment>()) {
            auto& equipment = follower.get_mut<gameplay::Equipment>();
            if (equipment.weapon == swap.from) {
                equipment.weapon = swap.to;
            }
        }
        LOG_INFO("É7: {} forge upgrade {} -> {}", actor->editorId,
                 swap.from.toString(), swap.to.toString());
    }
    if (ctx.texts) {
        toast(ctx, ctx.texts->format("follower.forgeUpgraded",
                                     actor->displayName));
    }
}

// ---- É8: mort, tombe, enterrement --------------------------------------------

namespace {

// The grave guid namespace — the prefab-child derivation idiom (§2.11):
// combine(followerReference, kGraveNamespace) is stable forever.
const core::Guid kGraveNamespace =
    *core::Guid::fromString("6a1dc0de-e8e8-4000-8000-6772617665f0");

// The shared burial core: create the persistent grave reference at
// `gravePos` (pending layer + live spawn), move the corpse's inventory
// into it, remove the corpse (the picked-up-item idiom). Returns true
// when the corpse entity was destructed (NPC list refresh needed).
bool buryFollower(const FollowerContext& ctx, ecs::Entity corpse,
                  const Vec3& gravePos) {
    const data::ActorForm* actor = followerActorForm(ctx, corpse);
    if (!actor || !corpse.has<world::RefId>()) {
        return false;
    }
    const auto* grave =
        data::findByEditorId<gameplay::FurnitureForm>(ctx.forms, "Grave");
    if (!grave) {
        LOG_WARN("É8: no 'Grave' FurnitureForm in the data — burial skipped");
        return false;
    }
    const core::Guid corpseRef = corpse.get<world::RefId>().referenceId;
    if (!corpseRef.isValid()) {
        return false; // identity-less corpse: nothing to disable or derive
    }
    const core::Guid graveRef = FollowerController::graveGuidFor(corpseRef);

    // 1) The persistent record (§2.11: the generalized disableReference
    // materialization). Null cell = the persistent set — the grave
    // streams with nobody and survives every reload like the player.
    ctx.pendingSave.createReference(graveRef, grave->id, core::Guid {},
                                    gravePos);

    // 2) The live spawn — the spawnInitialWorld idiom through the scene's
    // Spawner (parent cell = none). Headless callers may skip it.
    ecs::Entity graveEntity {};
    if (ctx.spawnPersistent) {
        world::ReferenceForm reference;
        reference.id = graveRef;
        reference.baseForm = grave->id;
        reference.position = gravePos;
        graveEntity = ctx.spawnPersistent(reference);
    }

    // 3) The corpse's whole inventory moves into the grave (the container
    // screen later deposits/retrieves through the same Inventory).
    if (graveEntity.is_alive()) {
        if (!graveEntity.has<gameplay::Inventory>()) {
            graveEntity.set<gameplay::Inventory>({});
        }
        if (corpse.has<gameplay::Inventory>()) {
            gameplay::transferAllItems(
                corpse.get_mut<gameplay::Inventory>(),
                graveEntity.get_mut<gameplay::Inventory>());
        }
        // Capture NOW: the grave's creates record + its SavedItemForm
        // children live in the pending layer from this frame on.
        ctx.pendingSave.captureEntity(graveEntity, ctx.forms, ctx.gameTags);
    }

    // 4) The corpse leaves the world the picked-up-item way: enabled =
    // false in the pending layer (his authored record never respawns
    // him), then the entity goes.
    ctx.pendingSave.disableReference(corpseRef, ctx.forms, corpse);
    corpse.destruct();

    if (ctx.texts) {
        toast(ctx, ctx.texts->format("follower.buried", actor->displayName));
    }
    LOG_INFO("É8: {} buried — grave {} at ({:.1f}, {:.1f}, {:.1f})",
             actor->editorId, graveRef.toString(), gravePos.x, gravePos.y,
             gravePos.z);
    return true;
}

} // namespace

core::Guid FollowerController::graveGuidFor(
    const core::Guid& followerReference) {
    return core::Guid::combine(followerReference, kGraveNamespace);
}

str FollowerController::graveOwnerName(const data::FormDatabase& forms,
                                       const core::Guid& graveReference) {
    // Recompute the derivation over the resolved references — the grave
    // guid IS the link, so no owner field needs persisting. References
    // are few (a hand-authored world); this runs on an [E] press.
    str name;
    data::forEach<world::ReferenceForm>(
        forms, [&](const world::ReferenceForm& reference) {
            if (core::Guid::combine(reference.id, kGraveNamespace) !=
                graveReference) {
                return;
            }
            const data::FormHandle baseHandle =
                forms.handleOf(reference.baseForm);
            const data::Form* base = forms.get(baseHandle);
            const reflect::TypeInfo* type = forms.typeOf(baseHandle);
            if (base && type &&
                type->isA(data::ActorForm::staticTypeInfo().id)) {
                name = static_cast<const data::ActorForm*>(base)->displayName;
            }
        });
    return name;
}

bool FollowerController::buryOnSpot(const FollowerContext& ctx,
                                    ecs::Entity corpse) {
    if (!corpse.is_alive() || !corpse.has<world::Transform>()) {
        return false;
    }
    return buryFollower(ctx, corpse, corpse.get<world::Transform>().position);
}

bool FollowerController::buryByContact(const FollowerContext& ctx,
                                       ecs::Entity partner) {
    // The partner's ActorForm identity — buryContact on the DEAD
    // follower's form points at it (É0 data).
    core::Guid partnerForm;
    if (partner.is_alive() && partner.has<world::RefId>()) {
        if (const data::Form* base =
                ctx.forms.get(partner.get<world::RefId>().base)) {
            partnerForm = base->id;
        }
    }
    if (partnerForm.isValid()) {
        for (const auto& npcPtr : ctx.npcDirector.npcs()) {
            const Npc& npc = *npcPtr;
            if (!npc.dead || !npc.entity.is_alive()) {
                continue;
            }
            const data::ActorForm* actor = followerActorForm(ctx, npc.entity);
            if (!actor || actor->buryContact != partnerForm) {
                continue;
            }
            // The authored grave spot (buryMarker reference), grounded on
            // the terrain like every NPC build; fallback: where he lies.
            Vec3 gravePos = npc.entity.get<world::Transform>().position;
            if (const auto* marker = ctx.forms.find<world::ReferenceForm>(
                    actor->buryMarker)) {
                gravePos = marker->position;
                gravePos.y = render::terrain::height(
                    ctx.terrainParams, gravePos.x, gravePos.z);
            }
            return buryFollower(ctx, npc.entity, gravePos);
        }
    }
    // Nobody to bury: the option stays visible (v1 — no "follower X is
    // dead" condition kind), so the handler answers.
    if (ctx.texts) {
        toast(ctx, ctx.texts->get("follower.buryNone"));
    }
    return false;
}

void FollowerController::teleportNear(const Vec3& anchor,
                                      const render::TerrainParams& terrain,
                                      Npc& npc) {
    auto& transform = npc.entity.get_mut<world::Transform>();
    // A fixed side-step off the anchor (feel pass later), grounded the way
    // every NPC build is (NpcSpawner's terrain::height snap).
    Vec3 spot = anchor + Vec3 { 1.6f, 0.0f, -1.6f };
    spot.y = render::terrain::height(terrain, spot.x, spot.z);
    transform.position = spot;
    npc.path.clear();
    npc.pathIndex = 0;
    npc.repathTimer = 0.0f;
    npc.speed = 0.0f;
}

} // namespace game
