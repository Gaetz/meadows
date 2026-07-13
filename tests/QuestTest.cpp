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

// 8.7c — data-driven quest starts: QuestForm.startEvent + startQuestsOn.
TEST_CASE("startQuestsOn begins matching quests once, never restarts") {
    data::FormDatabase db = buildQuestDb();
    // Give the quest a start event (the dialogue option fires it).
    auto* quest = const_cast<QuestForm*>(db.find<QuestForm>(kQuest));
    REQUIRE(quest != nullptr);
    quest->startEvent = "OnAcceptSlay";

    QuestLog log;
    // A foreign event starts nothing.
    const auto none = startQuestsOn(
        log, db, { gameplay::eventKind("OnSomethingElse") });
    CHECK(none.empty());
    CHECK_FALSE(log.quests.contains(kQuest));

    // The matching event starts it (and reports it to the caller).
    const auto started =
        startQuestsOn(log, db, { gameplay::eventKind("OnAcceptSlay") });
    REQUIRE(started.size() == 1);
    CHECK(started[0]->id == kQuest);
    CHECK(isActive(log, kQuest));
    CHECK(questState(log, kQuest) == kStart);

    // Re-firing never re-begins — active or finished, the entry stays.
    const auto again =
        startQuestsOn(log, db, { gameplay::eventKind("OnAcceptSlay") });
    CHECK(again.empty());
    log.quests[kQuest].status = QuestStatus::Succeeded;
    const auto after =
        startQuestsOn(log, db, { gameplay::eventKind("OnAcceptSlay") });
    CHECK(after.empty());
    CHECK(questStatus(log, kQuest) == QuestStatus::Succeeded);
}

TEST_CASE("startQuestsOn ignores quests without a startEvent") {
    const data::FormDatabase db = buildQuestDb(); // startEvent = ""
    QuestLog log;
    const auto started =
        startQuestsOn(log, db, { gameplay::eventKind("OnAcceptSlay") });
    CHECK(started.empty());
    CHECK(log.quests.empty());
}

// The failure path (dev question 2026-07-10): a branch into a
// kind=Failure state — e.g. wired to a "betray the village" dialogue
// option — fails the quest exactly like Success succeeds it.
TEST_CASE("a branch into a Failure state fails the quest") {
    data::FormDatabase db = buildQuestDb();
    const Guid kFailed =
        *Guid::fromString("90000000-0000-4000-8000-00000000000c");
    const Guid kBetrayBranch =
        *Guid::fromString("90000000-0000-4000-8000-0000000000c2");
    const Guid kBetrayTask =
        *Guid::fromString("90000000-0000-4000-8000-0000000000d2");

    auto failed = std::make_unique<QuestStateForm>();
    failed->id = kFailed;
    failed->quest = kQuest;
    failed->kind = "Failure";
    db.add(std::move(failed), QuestStateForm::staticTypeInfo());

    auto branch = std::make_unique<QuestBranchForm>();
    branch->id = kBetrayBranch;
    branch->state = kStart;
    branch->destination = kFailed;
    db.add(std::move(branch), QuestBranchForm::staticTypeInfo());

    auto task = std::make_unique<QuestTaskForm>();
    task->id = kBetrayTask;
    task->branch = kBetrayBranch;
    task->event = "OnBetray"; // what the dialogue option would fire
    db.add(std::move(task), QuestTaskForm::staticTypeInfo());

    gameplay::GameplayTagRegistry tags;
    QuestLog log;
    beginQuest(log, db, kQuest);
    REQUIRE(isActive(log, kQuest));

    onQuestEvent(log, db, { gameplay::eventKind("OnBetray") }, tags);
    CHECK(questStatus(log, kQuest) == QuestStatus::Failed);
    CHECK_FALSE(isActive(log, kQuest));
    CHECK(questState(log, kQuest) == kFailed);
    // And a failed quest never restarts through its startEvent (8.7c).
    auto* quest = const_cast<QuestForm*>(db.find<QuestForm>(kQuest));
    quest->startEvent = "OnAcceptSlay";
    const auto started =
        startQuestsOn(log, db, { gameplay::eventKind("OnAcceptSlay") });
    CHECK(started.empty());
}

TEST_CASE("setQuestState jumps stages: enlists, re-opens, finishes (setstage)") {
    const data::FormDatabase db = buildQuestDb();
    QuestLog log;

    // Never taken: setstage enlists the quest directly on the target state.
    CHECK(setQuestState(log, db, kQuest, kStart));
    CHECK(isActive(log, kQuest));
    CHECK(questState(log, kQuest) == kStart);

    // Jump to the Success state: finishes exactly like a normal transition.
    CHECK(setQuestState(log, db, kQuest, kDone));
    CHECK(questStatus(log, kQuest) == QuestStatus::Succeeded);
    CHECK_FALSE(isActive(log, kQuest));

    // Jump BACK re-opens it and clears task progress.
    log.quests[kQuest].taskProgress[kTask] = 1;
    CHECK(setQuestState(log, db, kQuest, kStart));
    CHECK(isActive(log, kQuest));
    CHECK(taskProgress(log, kQuest, kTask) == 0);

    // A state of ANOTHER quest (or an unknown guid) is refused, no change.
    const Guid other =
        *Guid::fromString("90000000-0000-4000-8000-0000000000ee");
    CHECK_FALSE(setQuestState(log, db, other, kDone));
    CHECK_FALSE(setQuestState(log, db, kQuest, other));
    CHECK(questState(log, kQuest) == kStart);
}
