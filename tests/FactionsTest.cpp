#include <doctest/doctest.h>

#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/faction/Factions.hpp"

using core::Guid;
using namespace gameplay;

namespace {

void addRelation(data::FormDatabase& db, const char* id, const char* a,
                 const char* b, const char* relation) {
    auto form = std::make_unique<FactionRelationForm>();
    form->id = *Guid::fromString(id);
    form->factionA = a;
    form->factionB = b;
    form->relation = relation;
    db.add(std::move(form), FactionRelationForm::staticTypeInfo());
}

} // namespace

TEST_CASE("factions: membership is gameplay tags; relations come from the table") {
    GameplayTagRegistry tags;
    const GameplayTag guard = tags.registerTag("Faction.CityGuard");
    const GameplayTag bandit = tags.registerTag("Faction.Bandit");
    const GameplayTag citizen = tags.registerTag("Faction.Citizen");

    // Membership: an entity simply owns its faction tag.
    TagContainer guardMember;
    guardMember.add(guard, tags);
    CHECK(guardMember.has(guard));
    CHECK(guardMember.has(*tags.find("Faction"))); // ancestor-aware

    data::FormDatabase db;
    addRelation(db, "fa000000-0000-4000-8000-000000000001",
                "Faction.CityGuard", "Faction.Bandit", "enemy");
    addRelation(db, "fa000000-0000-4000-8000-000000000002",
                "Faction.CityGuard", "Faction.Citizen", "ally");

    const FactionRelations relations = FactionRelations::build(db, tags);

    CHECK(relations.standingBetween(guard, bandit) == FactionStanding::Enemy);
    CHECK(relations.standingBetween(bandit, guard) == FactionStanding::Enemy); // symmetric
    CHECK(relations.standingBetween(guard, citizen) == FactionStanding::Ally);
    // Unlisted pair defaults to Neutral.
    CHECK(relations.standingBetween(bandit, citizen) == FactionStanding::Neutral);
}
