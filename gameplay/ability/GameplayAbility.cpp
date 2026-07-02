#include "gameplay/ability/GameplayAbility.hpp"

#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/condition/Condition.hpp"

namespace gameplay {

namespace {

// Whether the caster can pay `cost`, per the ability's cost policy. Only simple
// negative add costs are gated; anything else is always allowed.
//   permissive — activate with any reserve > 0 (spend what you have; overdraw
//                clamps to 0). Default for energy (stamina): a roll costing 25 is
//                allowed at 12 energy, but not at 0 (that is the exhaustion gate).
//   strict     — require the full cost (current + magnitude >= 0). Default for
//                magic (essence) and any non-energy resource.
// An empty policy resolves by resource: energy → permissive, else → strict.
bool canAfford(const AbilitySystem& caster, const EffectForm& cost,
               const str& policy) {
    if (cost.op != "add" || cost.magnitude >= 0.0f) {
        return true;
    }
    const f32 current = currentValueOf(caster, attr(cost.attribute));
    const bool permissive = policy == "permissive" ||
                            (policy.empty() && cost.attribute == "energy");
    if (permissive) {
        return current > 0.0f;
    }
    return current + cost.magnitude >= 0.0f;
}

} // namespace

void registerGameplayFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<EffectForm>();
    registry.registerFormType<AbilityForm>();
    registry.registerFormType<ConditionForm>();
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

    // Data-driven activation conditions (4c), when an evaluation context is
    // provided — the ability's ConditionForms must all pass.
    if (ctx.eval && !conditionsPass(ctx.forms, ability.id, *ctx.eval)) {
        return false;
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
    if (cost && !canAfford(casterSystem, *cost, ability.costPolicy)) {
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
