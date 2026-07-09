#pragma once

#include <functional>
#include <string_view>

#include "data/forms/Form.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/inventory/Inventory.hpp"

namespace data {
class FormDatabase;
}

namespace gameplay {

// One condition clause (a Form, moddable). A node's conditions = all the
// ConditionForms whose `parent` is that node's id, ANDed (NarrativePro's
// Conditions[] pattern, decomposed into patchable records). The condition
// evaluator is THE shared predicate engine — abilities, quests, dialogue, AI.
// New clause kinds are added by extending evaluateClause (e.g. QuestStage,
// FactionStanding land with quests/factions).
struct ConditionForm : data::Form {
    core::Guid parent;      // the gated node (ability / quest stage / dialogue node)
    str kind { "HasTag" };  // HasTag | AttributeAtLeast | AttributeAtMost | HasItem | Lua
    str tag;                // HasTag
    str attribute;          // AttributeAtLeast / AttributeAtMost
    f32 value { 0.0f };     // threshold (attributes) / min count (HasItem)
    core::Guid item;        // HasItem
    str lua;                // Lua (a boolean predicate expression)
    bool negate { false };  // flips the clause result

    REFLECT_BEGIN(ConditionForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(kind)
        REFLECT_FIELD(tag)
        REFLECT_FIELD(attribute)
        REFLECT_FIELD(value)
        REFLECT_FIELD(item)
        REFLECT_FIELD(lua)
        REFLECT_FIELD(negate)
    REFLECT_END()
};

// What a condition evaluates against; pointers may be null (a clause needing a
// missing source fails). The Lua escape is a CALLBACK supplied by the script
// layer — gameplay must not depend on the VM (no cycle).
struct EvalContext {
    const AbilitySystem* abilitySystem { nullptr }; // tags + current attributes
    const Inventory* inventory { nullptr };
    const GameplayTagRegistry* tags { nullptr };
    std::function<bool(std::string_view predicate)> luaPredicate;
};

bool evaluateClause(const ConditionForm& clause, const EvalContext& context);

// One-line human reading of a clause (chantier 8.9 — the editor's
// condition builder and the dialogue hierarchy): "if not tag
// Faction.Hostile", "if health >= 50"… Pure (no database: the item guid
// stays a guid; the UI resolves names itself).
str conditionSummary(const ConditionForm& clause);

// True if every ConditionForm with parent == `node` passes (true if there are
// none — an ungated node is always allowed).
bool conditionsPass(const data::FormDatabase& forms, const core::Guid& node,
                    const EvalContext& context);

} // namespace gameplay
