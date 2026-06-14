#include "gameplay/faction/Factions.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

namespace {

FactionStanding parseStanding(const str& relation) {
    if (relation == "enemy") {
        return FactionStanding::Enemy;
    }
    if (relation == "ally") {
        return FactionStanding::Ally;
    }
    return FactionStanding::Neutral;
}

u64 pairKey(GameplayTag a, GameplayTag b) {
    const u32 lo = std::min(a.id, b.id);
    const u32 hi = std::max(a.id, b.id);
    return (static_cast<u64>(hi) << 32) | lo;
}

} // namespace

void registerFactionFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<FactionRelationForm>();
}

FactionRelations FactionRelations::build(const data::FormDatabase& forms,
                                         const GameplayTagRegistry& tags) {
    FactionRelations table;
    const u32 typeId = FactionRelationForm::staticTypeInfo().id;
    for (u32 value = 1; value <= forms.count(); ++value) {
        const data::FormHandle handle { value };
        const reflect::TypeInfo* type = forms.typeOf(handle);
        const data::Form* form = forms.get(handle);
        if (!type || !form || !type->isA(typeId)) {
            continue;
        }
        const auto* relation = static_cast<const FactionRelationForm*>(form);
        const auto a = tags.find(relation->factionA);
        const auto b = tags.find(relation->factionB);
        if (a && b) {
            table.standings[pairKey(*a, *b)] = parseStanding(relation->relation);
        }
    }
    return table;
}

FactionStanding FactionRelations::standingBetween(GameplayTag a,
                                                  GameplayTag b) const {
    const auto it = standings.find(pairKey(a, b));
    return it != standings.end() ? it->second : FactionStanding::Neutral;
}

} // namespace gameplay
