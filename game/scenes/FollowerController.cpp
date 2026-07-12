#include "game/scenes/FollowerController.hpp"

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp" // data::ActorForm
#include "data/forms/FormDatabase.hpp"
#include "engine/core/Log.hpp"
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "game/SaveGame.hpp"                        // PendingSaveLayer
#include "game/scenes/NpcDirector.hpp"              // Npc, NpcDirector
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/actors/ActorState.hpp" // gameplay::FollowerState
#include "gameplay/actors/Followers.hpp"  // followTuning, adoptOnHit (É2)
#include "gameplay/event/EventBus.hpp"    // gameplay::Event (É2 aggro)
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

} // namespace

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
    state.followerActive = true;
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
        if (npc.dead || !npc.entity.is_alive() ||
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
        if (npc.dead || !npc.entity.is_alive()) {
            continue;
        }
        roles.self = npc.entity.id();
        roles.selfFollower = isActiveFollower(npc);
        roles.selfHostile = npc.hostile;
        // "Live target" = the adopted entity still stands (alive AND its
        // director record — if it has one — isn't dead yet).
        bool liveTarget = false;
        if (npc.combatTarget.id() != 0 && npc.combatTarget.is_alive()) {
            liveTarget = true;
            for (const auto& other : ctx.npcDirector.npcs()) {
                if (other->entity.id() == npc.combatTarget.id()) {
                    liveTarget = !other->dead;
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
