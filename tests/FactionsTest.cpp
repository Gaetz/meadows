#include <doctest/doctest.h>

#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "engine/ecs/World.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/actors/ActorState.hpp"
#include "gameplay/faction/Factions.hpp"
#include "gameplay/save/SaveState.hpp"

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

TEST_CASE("per-faction bounty: attribution, guard gate math, legacy total") {
    // The witness's faction holds the
    // slice; an unattributed total (old saves / factionless witness)
    // counts toward EVERY faction — no amnesty on migration.
    gameplay::GameplayTagRegistry tags;
    const gameplay::GameplayTag guards = tags.registerTag("Faction.VillageGuard");
    const gameplay::GameplayTag bandits = tags.registerTag("Faction.Bandits");

    gameplay::Bounty bounty;
    gameplay::addBounty(bounty, guards, 40.0f);
    CHECK(bounty.bounty == doctest::Approx(40.0f));
    CHECK(gameplay::bountyToward(bounty, guards) == doctest::Approx(40.0f));
    CHECK(gameplay::bountyToward(bounty, bandits) == doctest::Approx(0.0f));

    gameplay::addBounty(bounty, guards, 40.0f); // same faction accumulates
    CHECK(bounty.perFaction.size() == 1);
    CHECK(gameplay::bountyToward(bounty, guards) == doctest::Approx(80.0f));

    // A factionless witness raises the unattributed remainder: everyone.
    gameplay::addBounty(bounty, {}, 25.0f);
    CHECK(bounty.bounty == doctest::Approx(105.0f));
    CHECK(gameplay::unattributedBounty(bounty) == doctest::Approx(25.0f));
    CHECK(gameplay::bountyToward(bounty, guards) == doctest::Approx(105.0f));
    CHECK(gameplay::bountyToward(bounty, bandits) == doctest::Approx(25.0f));

    // A legacy save restores ONLY the total: every faction reacts.
    gameplay::Bounty legacy;
    legacy.bounty = 60.0f;
    CHECK(gameplay::bountyToward(legacy, guards) == doctest::Approx(60.0f));
    CHECK(gameplay::bountyToward(legacy, bandits) == doctest::Approx(60.0f));
}

TEST_CASE("per-faction bounty: capture/apply round-trips the slices") {
    gameplay::GameplayTagRegistry tags;
    const gameplay::GameplayTag guards = tags.registerTag("Faction.VillageGuard");

    ecs::World world;
    ecs::Entity entity = world.create();
    gameplay::Bounty bounty;
    gameplay::addBounty(bounty, guards, 40.0f);
    gameplay::addBounty(bounty, {}, 10.0f); // unattributed part
    entity.set<gameplay::Bounty>(bounty);

    const core::Guid refGuid =
        *core::Guid::fromString("cc330000-0000-4000-8000-0000000000aa");
    const vector<data::Record> records =
        gameplay::captureActor(entity, refGuid, tags);
    gameplay::SavedStatsForm stats;
    vector<gameplay::SavedBountyForm> rows;
    for (const data::Record& record : records) {
        if (record.typeId == gameplay::SavedStatsForm::staticTypeInfo().id) {
            stats = gameplay::formFromRecord<gameplay::SavedStatsForm>(record);
        } else if (record.typeId ==
                   gameplay::SavedBountyForm::staticTypeInfo().id) {
            rows.push_back(
                gameplay::formFromRecord<gameplay::SavedBountyForm>(record));
        }
    }
    CHECK(stats.bounty == doctest::Approx(50.0f)); // the legacy total field
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].faction == "Faction.VillageGuard");
    CHECK(rows[0].amount == doctest::Approx(40.0f));

    ecs::Entity reloaded = world.create();
    reloaded.set<gameplay::Bounty>({});
    gameplay::SavedActorRecords saved;
    saved.stats = &stats;
    for (const gameplay::SavedBountyForm& row : rows) {
        saved.bounties.push_back(&row);
    }
    gameplay::applySavedState(reloaded, saved, tags);
    const auto& restored = reloaded.get<gameplay::Bounty>();
    CHECK(restored.bounty == doctest::Approx(50.0f));
    CHECK(gameplay::bountyToward(restored, guards) == doctest::Approx(50.0f));
    CHECK(gameplay::unattributedBounty(restored) == doctest::Approx(10.0f));
}

TEST_CASE("per-faction bounty: the fine settles the arresting faction only") {
    gameplay::GameplayTagRegistry tags;
    const gameplay::GameplayTag guards = tags.registerTag("Faction.VillageGuard");
    const gameplay::GameplayTag militia = tags.registerTag("Faction.Militia");

    gameplay::Bounty bounty;
    gameplay::addBounty(bounty, guards, 40.0f);
    gameplay::addBounty(bounty, militia, 30.0f);
    gameplay::addBounty(bounty, {}, 10.0f); // unattributed

    // Paying the guard clears HIS slice + the unattributed part; the
    // militia's slice survives (its guards stay hostile, Wanted holds).
    gameplay::clearBountyToward(bounty, guards);
    CHECK(bounty.bounty == doctest::Approx(30.0f));
    CHECK(gameplay::bountyToward(bounty, guards) == doctest::Approx(0.0f));
    CHECK(gameplay::bountyToward(bounty, militia) == doctest::Approx(30.0f));
    CHECK(gameplay::unattributedBounty(bounty) == doctest::Approx(0.0f));

    // An unknown arrester (invalid faction) wipes everything — the
    // legacy fallback.
    gameplay::clearBountyToward(bounty, {});
    CHECK(bounty.bounty == doctest::Approx(0.0f));
    CHECK(bounty.perFaction.empty());
}
