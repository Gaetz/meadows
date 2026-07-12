#include "gameplay/condition/Condition.hpp"

#include <cstdio>
#include <string_view>
#include <unordered_map>

#include "data/forms/FormDatabase.hpp"
#include "gameplay/actors/ActorState.hpp" // FollowerState (É4 partner clause)

namespace gameplay {

namespace {

// One evaluator per clause kind. Registering a new kind is adding an entry to
// the table below — the dispatch stays open for extension, closed for
// modification (OCP), with no growing if/else chain.
bool hasTag(const ConditionForm& clause, const EvalContext& ctx) {
    if (!ctx.abilitySystem || !ctx.tags) {
        return false;
    }
    const auto tag = ctx.tags->find(clause.tag);
    return tag && ctx.abilitySystem->tags.has(*tag);
}

bool attributeAtLeast(const ConditionForm& clause, const EvalContext& ctx) {
    return ctx.abilitySystem &&
           currentValueOf(*ctx.abilitySystem, attr(clause.attribute)) >= clause.value;
}

bool attributeAtMost(const ConditionForm& clause, const EvalContext& ctx) {
    return ctx.abilitySystem &&
           currentValueOf(*ctx.abilitySystem, attr(clause.attribute)) <= clause.value;
}

bool hasItem(const ConditionForm& clause, const EvalContext& ctx) {
    if (!ctx.inventory) {
        return false;
    }
    const i32 needed = clause.value > 0.0f ? static_cast<i32>(clause.value) : 1;
    return itemCount(*ctx.inventory, clause.item) >= needed;
}

bool luaPredicate(const ConditionForm& clause, const EvalContext& ctx) {
    return ctx.luaPredicate && ctx.luaPredicate(clause.lua);
}

// FOLLOWERS É4: reads the DIALOGUE PARTNER's affinity (EvalContext.
// partnerFollower — the player context carries no affinity). Fails closed
// when no partner (or a non-follower partner) filled the context.
bool followerAffinityAtLeast(const ConditionForm& clause,
                             const EvalContext& ctx) {
    return ctx.partnerFollower &&
           ctx.partnerFollower->followerAffinity >= clause.value;
}

using ClauseFn = bool (*)(const ConditionForm&, const EvalContext&);
const std::unordered_map<std::string_view, ClauseFn> kClauseEvaluators {
    { "HasTag", &hasTag },
    { "AttributeAtLeast", &attributeAtLeast },
    { "AttributeAtMost", &attributeAtMost },
    { "HasItem", &hasItem },
    { "Lua", &luaPredicate },
    { "FollowerAffinityAtLeast", &followerAffinityAtLeast }, // É4
};

} // namespace

bool evaluateClause(const ConditionForm& clause, const EvalContext& context) {
    const auto it = kClauseEvaluators.find(clause.kind);
    const bool result = it != kClauseEvaluators.end()
                            ? it->second(clause, context)
                            : false;
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

str conditionSummary(const ConditionForm& clause) {
    const str prefix = clause.negate ? "if not " : "if ";
    const auto num = [](f32 v) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%g", v);
        return str { buf };
    };
    if (clause.kind == "HasTag") {
        return prefix + "tag " + (clause.tag.empty() ? "(?)" : clause.tag);
    }
    if (clause.kind == "AttributeAtLeast") {
        return prefix +
               (clause.attribute.empty() ? "(?)" : clause.attribute) +
               " >= " + num(clause.value);
    }
    if (clause.kind == "AttributeAtMost") {
        return prefix +
               (clause.attribute.empty() ? "(?)" : clause.attribute) +
               " <= " + num(clause.value);
    }
    if (clause.kind == "HasItem") {
        return prefix + "has item x" + num(clause.value < 1.0f
                                               ? 1.0f
                                               : clause.value);
    }
    if (clause.kind == "FollowerAffinityAtLeast") {
        return prefix + "affinity >= " + num(clause.value);
    }
    if (clause.kind == "Lua") {
        const str expr = clause.lua.size() > 24
                             ? clause.lua.substr(0, 24) + "..."
                             : clause.lua;
        return prefix + "lua: " + (expr.empty() ? "(?)" : expr);
    }
    return prefix + clause.kind + " (?)";
}

} // namespace gameplay
