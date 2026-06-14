#pragma once

#include <unordered_map>

#include "data/forms/Form.hpp"
#include "gameplay/ability/GameplayTags.hpp"

namespace data {
class FormDatabase;
class FormTypeRegistry;
}

namespace gameplay {

enum class FactionStanding { Enemy, Neutral, Ally };

// One faction-to-faction relation (a Form, moddable). The relations "table" is
// simply the set of these records; `FactionRelations` aggregates them. Faction
// MEMBERSHIP is expressed as gameplay tags on the entity (§6.1) — there is no
// per-entity faction component. factionA/B are faction tag names
// ("Faction.CityGuard"); `relation` is "enemy" | "neutral" | "ally".
struct FactionRelationForm : data::Form {
    str factionA;
    str factionB;
    str relation { "neutral" };

    REFLECT_BEGIN(FactionRelationForm, data::Form)
        REFLECT_FIELD(factionA)
        REFLECT_FIELD(factionB)
        REFLECT_FIELD(relation)
    REFLECT_END()
};

void registerFactionFormTypes(data::FormTypeRegistry& registry);

// Resolved relations table built from the FactionRelationForms in a database
// (layered like any Form, §5). Read-only; rebuild after re-resolution.
class FactionRelations {
public:
    static FactionRelations build(const data::FormDatabase& forms,
                                  const GameplayTagRegistry& tags);

    // Symmetric; Neutral if the pair is not in the table.
    FactionStanding standingBetween(GameplayTag a, GameplayTag b) const;

private:
    std::unordered_map<u64, FactionStanding> standings; // canonical (min,max) key
};

} // namespace gameplay
