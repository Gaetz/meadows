#pragma once

#include <unordered_map>

#include "data/forms/Form.hpp"
#include "data/plugins/Record.hpp"
#include "gameplay/event/EventBus.hpp"

namespace data {
class FormDatabase;
class FormTypeRegistry;
}
namespace gameplay {
class GameplayTagRegistry;
}

// Quests — NarrativePro's state machine, decomposed into
// individually-patchable Form records linked by id (States → Branches → Tasks).
// A branch completes when all its tasks are done → the quest enters the branch's
// destination state. Success/Failure states finish the quest. Tasks progress on
// gameplay events (EventBus). Runtime state lives in `QuestLog`.

namespace quest {

struct QuestForm : data::Form {
    str displayName;
    core::Guid startState;
    // The event that STARTS this quest (e.g. fired by a dialogue
    // option) — data-driven quest acquisition, no C++ wiring per quest.
    // "" = started by code/script only.
    str startEvent;
    // Paid to the player ONCE, when the quest first succeeds —
    // data-driven rewards. 0/null = no reward.
    core::Guid rewardItem;
    i32 rewardCount { 0 };

    REFLECT_BEGIN(QuestForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(startState)
        REFLECT_FIELD(startEvent)
        REFLECT_FIELD(rewardItem)
        REFLECT_FIELD(rewardCount)
    REFLECT_END()
};

struct QuestStateForm : data::Form {
    core::Guid quest;
    str kind { "Regular" }; // Regular | Success | Failure

    REFLECT_BEGIN(QuestStateForm, data::Form)
        REFLECT_FIELD(quest)
        REFLECT_FIELD(kind)
    REFLECT_END()
};

struct QuestBranchForm : data::Form {
    core::Guid state;       // parent state
    core::Guid destination; // state entered when the branch's tasks complete

    REFLECT_BEGIN(QuestBranchForm, data::Form)
        REFLECT_FIELD(state)
        REFLECT_FIELD(destination)
    REFLECT_END()
};

struct QuestTaskForm : data::Form {
    core::Guid branch;   // parent branch
    str displayName;
    str event;           // event name that progresses it (e.g. "OnDeath")
    str filterTag;       // optional: the event's tag must be a descendant of this
    i32 required { 1 };

    REFLECT_BEGIN(QuestTaskForm, data::Form)
        REFLECT_FIELD(branch)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(event)
        REFLECT_FIELD(filterTag)
        REFLECT_FIELD(required)
    REFLECT_END()
};

void registerQuestFormTypes(data::FormTypeRegistry& registry);

enum class QuestStatus { Active, Succeeded, Failed };

struct QuestProgress {
    core::Guid currentState;
    std::unordered_map<core::Guid, i32> taskProgress; // task id → count
    QuestStatus status { QuestStatus::Active };
};

// The player's quest journal (runtime state).
struct QuestLog {
    std::unordered_map<core::Guid, QuestProgress> quests; // quest id → progress
};

void beginQuest(QuestLog& log, const data::FormDatabase& forms,
                const core::Guid& questId);

// Data-driven quest starts: begins every quest whose `startEvent`
// matches the event and which was never taken (a finished quest never
// restarts: the log keeps its entry). Returns the quests started this
// call, for the caller's toasts/tag sync.
vector<const QuestForm*> startQuestsOn(QuestLog& log,
                                       const data::FormDatabase& forms,
                                       const gameplay::Event& event);

// Advances the tasks of every active quest whose current state matches the
// event, taking a branch (and transitioning / finishing) when its tasks complete.
void onQuestEvent(QuestLog& log, const data::FormDatabase& forms,
                  const gameplay::Event& event,
                  const gameplay::GameplayTagRegistry& tags);

// Dev/console jump (the `setstage` command): forces the quest
// onto `stateId` — starts it if it was never taken, clears task progress,
// and a Success/Failure state kind finishes it exactly like a normal
// transition. False (no change) when the state is unknown or belongs to
// another quest.
bool setQuestState(QuestLog& log, const data::FormDatabase& forms,
                   const core::Guid& questId, const core::Guid& stateId);

bool isActive(const QuestLog& log, const core::Guid& questId);
core::Guid questState(const QuestLog& log, const core::Guid& questId);
i32 taskProgress(const QuestLog& log, const core::Guid& questId,
                 const core::Guid& taskId);
QuestStatus questStatus(const QuestLog& log, const core::Guid& questId);

// --- Save records. The QuestLog persists as ordinary
// plugin records (§2.4/§5), like everything else: one SavedQuestForm per
// entry, one SavedQuestTaskForm per progressed task. Deterministic guids
// (Guid::combine), so re-saving is idempotent.

struct SavedQuestForm : data::Form {
    core::Guid quest;
    core::Guid currentState;
    i32 status { 0 }; // QuestStatus

    REFLECT_BEGIN(SavedQuestForm, data::Form)
        REFLECT_FIELD(quest)
        REFLECT_FIELD(currentState)
        REFLECT_FIELD(status)
    REFLECT_END()
};

struct SavedQuestTaskForm : data::Form {
    core::Guid quest;
    core::Guid task;
    i32 progress { 0 };

    REFLECT_BEGIN(SavedQuestTaskForm, data::Form)
        REFLECT_FIELD(quest)
        REFLECT_FIELD(task)
        REFLECT_FIELD(progress)
    REFLECT_END()
};

// The QuestLog -> save records mirror (sorted by quest then task, §8).
vector<data::Record> captureQuestLog(const QuestLog& log);
// Rebuilds the log from a resolved database carrying a save layer.
void applySavedQuests(QuestLog& log, const data::FormDatabase& forms);

} // namespace quest
