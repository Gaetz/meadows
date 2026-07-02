#pragma once

#include "data/forms/Form.hpp"
#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"

namespace data {
class FormTypeRegistry;
}

namespace gameplay {

struct EvalContext;

// A minimal GameplayAbility (§6): an activatable action. Faithful to GAS in that
// **cost and cooldown are themselves GameplayEffects**. Phase 3 keeps it flat
// and data-only — no AbilityTasks, no latent `wait()`, no Lua (Phase 4). The
// "what it does" is the primary `effect` applied to the target; richer custom
// logic comes with scripting in Phase 4.
struct AbilityForm : data::Form {
    str requiredTag;       // the caster must have it to activate
    str blockedTag;        // the caster must NOT have it (e.g. Status.Stunned)
    core::Guid cost;       // EffectForm applied to self on activation (optional)
    core::Guid cooldown;   // EffectForm applied to self; its grantedTag IS the
                           // cooldown tag checked on re-activation (optional)
    core::Guid effect;     // primary EffectForm applied to the target (optional)
    str script;            // optional Lua coroutine run on activation (latent
                           // logic: wait(t), self:applyEffect(...), …) — Phase 4

    // How the cost is gated (see canAfford):
    //   ""            auto — energy costs are "permissive", others "strict".
    //   "permissive"  activate with ANY reserve of the cost resource (spend what
    //                 you have; overdraw clamps to 0). Empty (0) still blocks —
    //                 for energy that is the State.Exhausted gate. Stamina model.
    //   "strict"      require the FULL cost (current >= cost). Magic model.
    str costPolicy;

    REFLECT_BEGIN(AbilityForm, data::Form)
        REFLECT_FIELD(requiredTag)
        REFLECT_FIELD(blockedTag)
        REFLECT_FIELD(cost)
        REFLECT_FIELD(cooldown)
        REFLECT_FIELD(effect)
        REFLECT_FIELD(script)
        REFLECT_FIELD(costPolicy)
    REFLECT_END()
};

// Registers the gameplay Form types (EffectForm + AbilityForm). Call once at
// startup before loading plugins that define effects/abilities.
void registerGameplayFormTypes(data::FormTypeRegistry& registry);

// Records that an actor can activate an ability (bookkeeping; §6 "AbilitySystem
// owns granted abilities").
void grantAbility(AbilitySystem& system, const core::Guid& ability);

// Resolved context an activation needs: where to look up effect Forms, and the
// tag vocabulary.
struct AbilityContext {
    const data::FormDatabase& forms;
    const GameplayTagRegistry& tags;
    // Optional: when set, the caster's ConditionForms (parent == ability.id)
    // must also pass for activation. Null = Phase-3 behaviour (tags/cost only).
    const EvalContext* eval { nullptr };
};

// Tries to activate `ability` from caster onto target (caster == target for a
// self-buff). Rejects (returns false, no state change) if an activation tag
// fails, the ability is on cooldown, or the cost is unaffordable. On success:
// applies cost + cooldown to the caster and the primary effect to the target.
bool tryActivate(const AbilityForm& ability,
                 AttributeSet& casterSet, AbilitySystem& casterSystem,
                 AttributeSet& targetSet, AbilitySystem& targetSystem,
                 const AbilityContext& ctx);

} // namespace gameplay
