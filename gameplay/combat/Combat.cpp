#include "gameplay/combat/Combat.hpp"

namespace gameplay {

void updateLifeState(AbilitySystem& system, const GameplayTagRegistry& registry) {
    const auto dead = registry.find("State.Dead");
    if (!dead) {
        return;
    }
    const bool atZero = currentValueOf(system, attr("health")) <= 0.0f;
    const bool tagged = system.tags.has(*dead);
    // FOLLOWERS É3 — the routing gate, in THE single life-state write
    // point: an actor carrying the Follower.Protected mirror (synced from
    // FollowerState.followerActive — active followers only) goes DOWNED
    // at 0 HP instead of dead. Real death still happens HERE: the
    // bleedout resolution lifts the protection and calls back in. With
    // neither tag registered/held the historical path is byte-identical
    // (bandits still just die — É3 iso-behavior).
    const auto downed = registry.find("State.Downed");
    const auto shield = registry.find("Follower.Protected");
    const bool isProtected = shield && system.tags.has(*shield);
    if (atZero && !tagged) {
        if (isProtected && downed) {
            if (!system.tags.has(*downed)) {
                system.tags.add(*downed, registry);
            }
        } else {
            system.tags.add(*dead, registry);
        }
    } else if (!atZero && tagged) {
        system.tags.remove(*dead, registry);
    }
    // Downed ⇔ (0 HP under protection): a heal above 0 stands him up, and
    // a corpse is never also downed (the real-death path relies on this).
    if (downed && system.tags.has(*downed) &&
        (!atZero || system.tags.has(*dead))) {
        system.tags.remove(*downed, registry);
    }
}

bool performAttack(const AbilityForm& ability,
                   AttributeSet& casterSet, AbilitySystem& casterSystem,
                   AttributeSet& targetSet, AbilitySystem& targetSystem,
                   const AbilityContext& ctx) {
    const bool activated = tryActivate(ability, casterSet, casterSystem,
                                       targetSet, targetSystem, ctx);
    if (activated) {
        updateLifeState(targetSystem, ctx.tags);
    }
    return activated;
}

} // namespace gameplay
