// Follower COMBAT: aggro adoption on hits, disengage on death,
// reviving a downed ally. Split from FollowerController.cpp (one TU
// per trade).
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
    // Remember the player's CURRENT target (the last hostile he
    // struck — the same aggro signal rule 4 reads) for the one-shot
    // "attack my target" adoption at command time.
    if (roles.sourcePlayer && targetNpc && targetNpc->hostile &&
        !targetNpc->dead) {
        playerTarget_ = event.target;
    }
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        Npc& npc = *npcPtr;
        // The downed adopt nothing — they are out of the fight.
        if (npc.dead || npc.downed || !npc.entity.is_alive()) {
            continue;
        }
        roles.self = npc.entity.id();
        roles.selfFollower = isActiveFollower(npc);
        roles.selfHostile = npc.hostile;
        // The defend-only stance turns rule 4 (player-initiative
        // adoption) off for THIS follower (gameplay::adoptOnHit).
        roles.defendOnly =
            roles.selfFollower &&
            gameplay::followerStance(
                npc.entity.get<gameplay::FollowerState>()) ==
                gameplay::FollowerStance::Defend;
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
        LOG_INFO("{} engages {} (aggro on hit)", npc.editorId,
                 adopted ? adopted->editorId : str { "?" });
    }
}

void FollowerController::onDeath(const FollowerContext& ctx,
                                 const gameplay::Event& event) {
    disengage(ctx, event.target.id());
}

void FollowerController::disengage(const FollowerContext& ctx, u64 gone) {
    if (gone == 0) {
        return;
    }
    for (auto& npcPtr : ctx.npcDirector.npcs()) {
        if (gameplay::disengageOnDeath(gone, npcPtr->combatTarget.id())) {
            npcPtr->combatTarget = ecs::Entity {};
        }
    }
    // A dead (or departed) hostile is no longer "my target".
    if (playerTarget_.id() == gone) {
        playerTarget_ = ecs::Entity {};
    }
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
    LOG_INFO("revived downed ally with '{}'", potion->editorId);
}
} // namespace game
