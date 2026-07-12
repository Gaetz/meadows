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
