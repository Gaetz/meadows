#include "gameplay/condition/Condition.hpp"

#include "data/forms/FormDatabase.hpp"

namespace gameplay {

bool evaluateClause(const ConditionForm& clause, const EvalContext& context) {
    bool result = false;
    if (clause.kind == "HasTag") {
        if (context.abilitySystem && context.tags) {
            if (const auto tag = context.tags->find(clause.tag)) {
                result = context.abilitySystem->tags.has(*tag);
            }
        }
    } else if (clause.kind == "AttributeAtLeast") {
        if (context.abilitySystem) {
            result = currentValueOf(*context.abilitySystem, attr(clause.attribute)) >=
                     clause.value;
        }
    } else if (clause.kind == "AttributeAtMost") {
        if (context.abilitySystem) {
            result = currentValueOf(*context.abilitySystem, attr(clause.attribute)) <=
                     clause.value;
        }
    } else if (clause.kind == "HasItem") {
        if (context.inventory) {
            const i32 needed =
                clause.value > 0.0f ? static_cast<i32>(clause.value) : 1;
            result = itemCount(*context.inventory, clause.item) >= needed;
        }
    } else if (clause.kind == "Lua") {
        result = context.luaPredicate ? context.luaPredicate(clause.lua) : false;
    }
    return clause.negate ? !result : result;
}

bool conditionsPass(const data::FormDatabase& forms, const core::Guid& node,
                    const EvalContext& context) {
    const u32 typeId = ConditionForm::staticTypeInfo().id;
    for (u32 value = 1; value <= forms.count(); ++value) {
        const data::FormHandle handle { value };
        const reflect::TypeInfo* type = forms.typeOf(handle);
        const data::Form* form = forms.get(handle);
        if (!type || !form || !type->isA(typeId)) {
            continue;
        }
        const auto* clause = static_cast<const ConditionForm*>(form);
        if (clause->parent != node) {
            continue;
        }
        if (!evaluateClause(*clause, context)) {
            return false;
        }
    }
    return true;
}

} // namespace gameplay
