#include "quest/Quest.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/save/SaveState.hpp"

namespace quest {

namespace {

// Is a branch complete (all its tasks at or past `required`, and it has tasks)?
bool branchComplete(const QuestIndex& index, const core::Guid& branchId,
                    const QuestProgress& progress) {
    const auto bucket = index.tasksOfBranch.find(branchId);
    if (bucket == index.tasksOfBranch.end() || bucket->second.empty()) {
        return false;
    }
    for (const QuestTaskForm* task : bucket->second) {
        const auto it = progress.taskProgress.find(task->id);
        const i32 current = it != progress.taskProgress.end() ? it->second : 0;
        if (current < task->required) {
            return false;
        }
    }
    return true;
}

// Advances every task of the current state that matches the event.
void progressTasks(const QuestIndex& index, QuestProgress& progress,
                   const gameplay::Event& event,
                   const gameplay::GameplayTagRegistry& tags) {
    const auto branches = index.branchesOfState.find(progress.currentState);
    if (branches == index.branchesOfState.end()) {
        return;
    }
    for (const QuestBranchForm* branch : branches->second) {
        const auto tasks = index.tasksOfBranch.find(branch->id);
        if (tasks == index.tasksOfBranch.end()) {
            continue;
        }
        for (const QuestTaskForm* task : tasks->second) {
            if (gameplay::eventKind(task->event) != event.kind) {
                continue;
            }
            if (!task->filterTag.empty()) {
                const auto filter = tags.find(task->filterTag);
                if (!filter || !tags.isA(event.tag, *filter)) {
                    continue;
                }
            }
            i32& count = progress.taskProgress[task->id];
            if (count < task->required) {
                ++count;
            }
        }
    }
}

// Takes the first complete branch of the current state, entering its
// destination (and finishing the quest if that state is Success/Failure).
void advanceCompletedBranch(const data::FormDatabase& forms,
                            const QuestIndex& index,
                            QuestProgress& progress) {
    core::Guid destination;
    bool transition = false;
    if (const auto branches =
            index.branchesOfState.find(progress.currentState);
        branches != index.branchesOfState.end()) {
        for (const QuestBranchForm* branch : branches->second) {
            if (branchComplete(index, branch->id, progress)) {
                transition = true;
                destination = branch->destination;
                break;
            }
        }
    }
    if (!transition) {
        return;
    }
    progress.currentState = destination;
    progress.taskProgress.clear();
    if (const QuestStateForm* state = forms.find<QuestStateForm>(destination)) {
        if (state->kind == "Success") {
            progress.status = QuestStatus::Succeeded;
        } else if (state->kind == "Failure") {
            progress.status = QuestStatus::Failed;
        }
    }
}

} // namespace

bool setQuestState(QuestLog& log, const data::FormDatabase& forms,
                   const core::Guid& questId, const core::Guid& stateId) {
    const QuestStateForm* state = forms.find<QuestStateForm>(stateId);
    if (!state || state->quest != questId) {
        return false;
    }
    QuestProgress& progress = log.quests[questId]; // enlists if never taken
    progress.currentState = stateId;
    progress.taskProgress.clear();
    progress.status = QuestStatus::Active; // jumping BACK re-opens it
    if (state->kind == "Success") {
        progress.status = QuestStatus::Succeeded;
    } else if (state->kind == "Failure") {
        progress.status = QuestStatus::Failed;
    }
    return true;
}

void registerQuestFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<QuestForm>();
    registry.registerFormType<QuestStateForm>();
    registry.registerFormType<QuestBranchForm>();
    registry.registerFormType<QuestTaskForm>();
    registry.registerFormType<SavedQuestForm>();
    registry.registerFormType<SavedQuestTaskForm>();
}

void beginQuest(QuestLog& log, const data::FormDatabase& forms,
                const core::Guid& questId) {
    const QuestForm* quest = forms.find<QuestForm>(questId);
    if (!quest) {
        return;
    }
    QuestProgress progress;
    progress.currentState = quest->startState;
    progress.status = QuestStatus::Active;
    log.quests[questId] = std::move(progress);
}

vector<const QuestForm*> startQuestsOn(QuestLog& log,
                                       const data::FormDatabase& forms,
                                       const QuestIndex& index,
                                       const gameplay::Event& event) {
    vector<const QuestForm*> started;
    const auto bucket = index.questsByStartEvent.find(event.kind);
    if (bucket == index.questsByStartEvent.end()) {
        return started;
    }
    for (const QuestForm* quest : bucket->second) {
        // Once in the log, never again — an abandoned/failed/succeeded
        // quest keeps its entry, so a re-fired event can't restart it.
        if (log.quests.contains(quest->id)) {
            continue;
        }
        beginQuest(log, forms, quest->id);
        started.push_back(quest);
    }
    return started;
}

QuestIndex buildQuestIndex(const data::FormDatabase& forms) {
    QuestIndex index;
    data::forEach<QuestBranchForm>(forms, [&](const QuestBranchForm& b) {
        index.branchesOfState[b.state].push_back(&b);
    });
    data::forEach<QuestTaskForm>(forms, [&](const QuestTaskForm& t) {
        index.tasksOfBranch[t.branch].push_back(&t);
    });
    data::forEach<QuestForm>(forms, [&](const QuestForm& q) {
        if (!q.startEvent.empty()) {
            index.questsByStartEvent[gameplay::eventKind(q.startEvent)]
                .push_back(&q);
        }
    });
    return index;
}

void onQuestEvent(QuestLog& log, const data::FormDatabase& forms,
                  const QuestIndex& index, const gameplay::Event& event,
                  const gameplay::GameplayTagRegistry& tags) {
    for (auto& [questId, progress] : log.quests) {
        if (progress.status != QuestStatus::Active) {
            continue;
        }
        progressTasks(index, progress, event, tags);
        advanceCompletedBranch(forms, index, progress);
    }
}

bool isActive(const QuestLog& log, const core::Guid& questId) {
    const auto it = log.quests.find(questId);
    return it != log.quests.end() && it->second.status == QuestStatus::Active;
}

core::Guid questState(const QuestLog& log, const core::Guid& questId) {
    const auto it = log.quests.find(questId);
    return it != log.quests.end() ? it->second.currentState : core::Guid {};
}

i32 taskProgress(const QuestLog& log, const core::Guid& questId,
                 const core::Guid& taskId) {
    const auto quest = log.quests.find(questId);
    if (quest == log.quests.end()) {
        return 0;
    }
    const auto task = quest->second.taskProgress.find(taskId);
    return task != quest->second.taskProgress.end() ? task->second : 0;
}

QuestStatus questStatus(const QuestLog& log, const core::Guid& questId) {
    const auto it = log.quests.find(questId);
    return it != log.quests.end() ? it->second.status : QuestStatus::Active;
}

vector<data::Record> captureQuestLog(const QuestLog& log) {
    constexpr core::Guid kSavedQuestNs { 0x5351554553542121ull,
                                         0x0000000000000005ull };
    vector<core::Guid> questIds;
    questIds.reserve(log.quests.size());
    for (const auto& [id, progress] : log.quests) {
        questIds.push_back(id);
    }
    std::sort(questIds.begin(), questIds.end());

    vector<data::Record> records;
    for (const core::Guid& questId : questIds) {
        const QuestProgress& progress = log.quests.at(questId);
        SavedQuestForm saved;
        saved.quest = questId;
        saved.currentState = progress.currentState;
        saved.status = static_cast<i32>(progress.status);
        records.push_back(gameplay::createRecord(
            saved, core::Guid::combine(kSavedQuestNs, questId)));

        vector<core::Guid> taskIds;
        taskIds.reserve(progress.taskProgress.size());
        for (const auto& [taskId, count] : progress.taskProgress) {
            if (count > 0) {
                taskIds.push_back(taskId);
            }
        }
        std::sort(taskIds.begin(), taskIds.end());
        for (const core::Guid& taskId : taskIds) {
            SavedQuestTaskForm task;
            task.quest = questId;
            task.task = taskId;
            task.progress = progress.taskProgress.at(taskId);
            records.push_back(gameplay::createRecord(
                task, core::Guid::combine(
                          core::Guid::combine(kSavedQuestNs, questId),
                          taskId)));
        }
    }
    return records;
}

void applySavedQuests(QuestLog& log, const data::FormDatabase& forms) {
    data::forEach<SavedQuestForm>(
        forms, [&](const SavedQuestForm& saved) {
            if (!saved.quest.isValid()) {
                return;
            }
            QuestProgress progress;
            progress.currentState = saved.currentState;
            progress.status = static_cast<QuestStatus>(saved.status);
            log.quests[saved.quest] = std::move(progress);
        });
    data::forEach<SavedQuestTaskForm>(
        forms, [&](const SavedQuestTaskForm& saved) {
            const auto it = log.quests.find(saved.quest);
            if (it != log.quests.end()) {
                it->second.taskProgress[saved.task] = saved.progress;
            }
        });
}

} // namespace quest
