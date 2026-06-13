#include "gameplay/combat/Combat.hpp"

namespace gameplay {

void updateLifeState(AbilitySystem& system, const GameplayTagRegistry& registry) {
    const auto dead = registry.find("State.Dead");
    if (!dead) {
        return;
    }
    const bool isDead = currentValueOf(system, attr("health")) <= 0.0f;
    const bool tagged = system.tags.has(*dead);
    if (isDead && !tagged) {
        system.tags.add(*dead, registry);
    } else if (!isDead && tagged) {
        system.tags.remove(*dead, registry);
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
