#include "gameplay/ability/GameplayAbility.hpp"

#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

namespace {

// A cost is affordable when applying its (negative add) modifier keeps the
// attribute >= 0. Only simple add costs are gated; anything else is allowed.
bool canAfford(const AbilitySystem& caster, const EffectForm& cost) {
    if (cost.op != "add" || cost.magnitude >= 0.0f) {
        return true;
    }
    return currentValueOf(caster, attr(cost.attribute)) + cost.magnitude >= 0.0f;
}

} // namespace

void registerGameplayFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<EffectForm>();
    registry.registerFormType<AbilityForm>();
}

void grantAbility(AbilitySystem& system, const core::Guid& ability) {
    system.grantedAbilities.push_back(ability);
}

bool tryActivate(const AbilityForm& ability,
                 AttributeSet& casterSet, AbilitySystem& casterSystem,
                 AttributeSet& targetSet, AbilitySystem& targetSystem,
                 const AbilityContext& ctx) {
    // Activation tag requirements (on the caster).
    if (!ability.requiredTag.empty()) {
        const auto tag = ctx.tags.find(ability.requiredTag);
        if (!tag || !casterSystem.tags.has(*tag)) {
            return false;
        }
    }
    if (!ability.blockedTag.empty()) {
        const auto tag = ctx.tags.find(ability.blockedTag);
        if (tag && casterSystem.tags.has(*tag)) {
            return false;
        }
    }

    // Cooldown: the cooldown effect's granted tag, if still present, blocks.
    const EffectForm* cooldown =
        ability.cooldown.isValid() ? ctx.forms.find<EffectForm>(ability.cooldown)
                                   : nullptr;
    if (cooldown && !cooldown->grantedTag.empty()) {
        if (const auto tag = ctx.tags.find(cooldown->grantedTag);
            tag && casterSystem.tags.has(*tag)) {
            return false; // on cooldown
        }
    }

    // Cost affordability.
    const EffectForm* cost =
        ability.cost.isValid() ? ctx.forms.find<EffectForm>(ability.cost)
                               : nullptr;
    if (cost && !canAfford(casterSystem, *cost)) {
        return false;
    }

    // Commit: pay the cost, start the cooldown, apply the primary effect.
    if (cost) {
        applyEffect(casterSet, casterSystem, *cost, ctx.tags);
    }
    if (cooldown) {
        applyEffect(casterSet, casterSystem, *cooldown, ctx.tags);
    }
    if (ability.effect.isValid()) {
        if (const EffectForm* primary = ctx.forms.find<EffectForm>(ability.effect)) {
            applyEffect(targetSet, targetSystem, *primary, ctx.tags);
        }
    }
    return true;
}

} // namespace gameplay
