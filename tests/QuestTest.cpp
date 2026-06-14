#include <doctest/doctest.h>

#include <memory>

#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/event/EventBus.hpp"
#include "quest/Quest.hpp"

using core::Guid;
using namespace quest;

namespace {

const Guid kQuest = *Guid::fromString("90000000-0000-4000-8000-000000000001");
const Guid kStart = *Guid::fromString("90000000-0000-4000-8000-00000000000a");
const Guid kDone = *Guid::fromString("90000000-0000-4000-8000-00000000000b");
const Guid kBranch = *Guid::fromString("90000000-0000-4000-8000-0000000000c1");
const Guid kTask = *Guid::fromString("90000000-0000-4000-8000-0000000000d1");

// A "slay 2 bandits" quest: Start --(kill 2, tagged Faction.Bandit)--> Success.
data::FormDatabase buildQuestDb() {
    data::FormDatabase db;
    auto quest = std::make_unique<QuestForm>();
    quest->id = kQuest;
    quest->startState = kStart;
    db.add(std::move(quest), QuestForm::staticTypeInfo());

    auto start = std::make_unique<QuestStateForm>();
    start->id = kStart;
    start->quest = kQuest;
    start->kind = "Regular";
    db.add(std::move(start), QuestStateForm::staticTypeInfo());

    auto done = std::make_unique<QuestStateForm>();
    done->id = kDone;
    done->quest = kQuest;
    done->kind = "Success";
    db.add(std::move(done), QuestStateForm::staticTypeInfo());

    auto branch = std::make_unique<QuestBranchForm>();
    branch->id = kBranch;
    branch->state = kStart;
    branch->destination = kDone;
    db.add(std::move(branch), QuestBranchForm::staticTypeInfo());

    auto task = std::make_unique<QuestTaskForm>();
    task->id = kTask;
    task->branch = kBranch;
    task->event = "OnDeath";
    task->filterTag = "Faction.Bandit";
    task->required = 2;
    db.add(std::move(task), QuestTaskForm::staticTypeInfo());
    return db;
}

} // namespace

TEST_CASE("quest: tasks progress on matching events and the quest completes") {
    const data::FormDatabase db = buildQuestDb();
    gameplay::GameplayTagRegistry tags;
    const gameplay::GameplayTag bandit = tags.registerTag("Faction.Bandit");
    const gameplay::GameplayTag guard = tags.registerTag("Faction.CityGuard");

    QuestLog log;
    beginQuest(log, db, kQuest);
    CHECK(isActive(log, kQuest));
    CHECK(questState(log, kQuest) == kStart);

    const auto banditDeath = [&] {
        return gameplay::Event { gameplay::eventKind("OnDeath"), {}, {}, bandit };
    };

    // Wrong tag → no progress.
    onQuestEvent(log, db, { gameplay::eventKind("OnDeath"), {}, {}, guard }, tags);
    CHECK(taskProgress(log, kQuest, kTask) == 0);

    // Wrong event kind → no progress.
    onQuestEvent(log, db, { gameplay::eventKind("OnHit"), {}, {}, bandit }, tags);
    CHECK(taskProgress(log, kQuest, kTask) == 0);

    onQuestEvent(log, db, banditDeath(), tags);
    CHECK(taskProgress(log, kQuest, kTask) == 1);
    CHECK(isActive(log, kQuest)); // need 2

    onQuestEvent(log, db, banditDeath(), tags);
    CHECK(questStatus(log, kQuest) == QuestStatus::Succeeded);
    CHECK_FALSE(isActive(log, kQuest));
    CHECK(questState(log, kQuest) == kDone);
}
