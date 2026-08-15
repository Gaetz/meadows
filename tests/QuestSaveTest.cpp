#include <doctest/doctest.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "quest/Quest.hpp"

// The quest log persists as ordinary save records —
// mid-quest progress survives capture -> TOML -> resolve -> apply.

namespace {

core::Guid guid(const char* text) {
    return *core::Guid::fromString(text);
}

// A two-kill quest so partial task progress is observable.
constexpr const char* kQuestToml = R"([plugin]
id = "60000000-0000-4000-8000-000000000001"
name = "quests"

[[records]]
form = "60000000-0000-4000-8000-000000000010"
type = "QuestForm"
new = true
[records.fields]
editorId = "Cull"
startState = "60000000-0000-4000-8000-000000000011"

[[records]]
form = "60000000-0000-4000-8000-000000000011"
type = "QuestStateForm"
new = true
[records.fields]
quest = "60000000-0000-4000-8000-000000000010"

[[records]]
form = "60000000-0000-4000-8000-000000000012"
type = "QuestStateForm"
new = true
[records.fields]
quest = "60000000-0000-4000-8000-000000000010"
kind = "Success"

[[records]]
form = "60000000-0000-4000-8000-000000000013"
type = "QuestBranchForm"
new = true
[records.fields]
state = "60000000-0000-4000-8000-000000000011"
destination = "60000000-0000-4000-8000-000000000012"

[[records]]
form = "60000000-0000-4000-8000-000000000014"
type = "QuestTaskForm"
new = true
[records.fields]
branch = "60000000-0000-4000-8000-000000000013"
event = "OnDeath"
filterTag = "Faction.Bandits"
required = 2
)";

const core::Guid kQuest = guid("60000000-0000-4000-8000-000000000010");
const core::Guid kTask = guid("60000000-0000-4000-8000-000000000014");

} // namespace

TEST_CASE("quest save: mid-quest progress round-trips through a plugin") {
    data::FormTypeRegistry types;
    quest::registerQuestFormTypes(types);
    const auto base = data::parsePluginToml(kQuestToml, types, "quests");
    REQUIRE(base.has_value());
    data::FormDatabase db;
    data::resolve({ &*base }, types, db);

    gameplay::GameplayTagRegistry tags;
    const auto bandits = tags.registerTag("Faction.Bandits");

    // Session 1: one of two kills done.
    quest::QuestLog log;
    quest::beginQuest(log, db, kQuest);
    quest::onQuestEvent(log, db,
                        { gameplay::eventKind("OnDeath"), {}, {}, bandits },
                        tags);
    REQUIRE(quest::taskProgress(log, kQuest, kTask) == 1);
    REQUIRE(quest::isActive(log, kQuest));

    // Capture -> TOML -> reparse -> resolve as the save layer.
    data::Plugin save;
    save.name = "slot";
    save.records = quest::captureQuestLog(log);
    const str toml = data::writePluginToml(save, types);
    const auto reparsed = data::parsePluginToml(toml, types, "slot");
    REQUIRE(reparsed.has_value());
    data::FormDatabase db2;
    data::resolve({ &*base, &*reparsed }, types, db2);

    // Session 2: fresh log, rebuilt from the resolved save records.
    quest::QuestLog restored;
    quest::applySavedQuests(restored, db2);
    CHECK(quest::isActive(restored, kQuest));
    CHECK(quest::taskProgress(restored, kQuest, kTask) == 1);
    CHECK(quest::questState(restored, kQuest) ==
          guid("60000000-0000-4000-8000-000000000011"));

    // The second kill finishes it — the machine picks up where it left.
    quest::onQuestEvent(restored, db2,
                        { gameplay::eventKind("OnDeath"), {}, {}, bandits },
                        tags);
    CHECK(quest::questStatus(restored, kQuest) ==
          quest::QuestStatus::Succeeded);

    // Determinism (§8): capturing twice yields identical record ids.
    const auto again = quest::captureQuestLog(log);
    REQUIRE(again.size() == save.records.size());
    REQUIRE(again.size() >= 2);
    CHECK(again[0].formId == save.records[0].formId);
    CHECK(again[1].formId == save.records[1].formId);
}
