#include "quest/Quest.hpp"

#include <algorithm>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/save/SaveState.hpp"

namespace quest {

namespace {

// (The per-type visitation is data::forEach — the local clone it duplicated
// is gone, audit U8-1.)

// Is a branch complete (all its tasks at or past `required`, and it has tasks)?
bool branchComplete(const data::FormDatabase& forms, const core::Guid& branchId,
                    const QuestProgress& progress) {
    bool hasTask = false;
    bool allDone = true;
    data::forEach<QuestTaskForm>(forms, [&](const QuestTaskForm& task) {
        if (task.branch != branchId) {
            return;
        }
        hasTask = true;
        const auto it = progress.taskProgress.find(task.id);
        const i32 current = it != progress.taskProgress.end() ? it->second : 0;
        if (current < task.required) {
            allDone = false;
        }
    });
    return hasTask && allDone;
}

// Advances every task of the current state that matches the event.
void progressTasks(const data::FormDatabase& forms, QuestProgress& progress,
                   const gameplay::Event& event,
                   const gameplay::GameplayTagRegistry& tags) {
    data::forEach<QuestBranchForm>(forms, [&](const QuestBranchForm& branch) {
        if (branch.state != progress.currentState) {
            return;
        }
        data::forEach<QuestTaskForm>(forms, [&](const QuestTaskForm& task) {
            if (task.branch != branch.id ||
                gameplay::eventKind(task.event) != event.kind) {
                return;
            }
            if (!task.filterTag.empty()) {
                const auto filter = tags.find(task.filterTag);
                if (!filter || !tags.isA(event.tag, *filter)) {
                    return;
                }
            }
            i32& count = progress.taskProgress[task.id];
            if (count < task.required) {
                ++count;
            }
        });
    });
}

// Takes the first complete branch of the current state, entering its
// destination (and finishing the quest if that state is Success/Failure).
void advanceCompletedBranch(const data::FormDatabase& forms,
                            QuestProgress& progress) {
    core::Guid destination;
    bool transition = false;
    data::forEach<QuestBranchForm>(forms, [&](const QuestBranchForm& branch) {
        if (!transition && branch.state == progress.currentState &&
            branchComplete(forms, branch.id, progress)) {
            transition = true;
            destination = branch.destination;
        }
    });
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

void registerQuestFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<QuestForm>();
    registry.registerFormType<QuestStateForm>();
    registry.registerFormType<QuestBranchForm>();
    registry.registerFormType<QuestTaskForm>();
    registry.registerFormType<SavedQuestForm>();     // chantier 6 A4
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
                                       const gameplay::Event& event) {
    vector<const QuestForm*> started;
    data::forEach<QuestForm>(forms, [&](const QuestForm& quest) {
        if (quest.startEvent.empty() ||
            gameplay::eventKind(quest.startEvent) != event.kind) {
            return;
        }
        // Once in the log, never again — an abandoned/failed/succeeded
        // quest keeps its entry, so a re-fired event can't restart it.
        if (log.quests.contains(quest.id)) {
            return;
        }
        beginQuest(log, forms, quest.id);
        started.push_back(&quest);
    });
    return started;
}

void onQuestEvent(QuestLog& log, const data::FormDatabase& forms,
                  const gameplay::Event& event,
                  const gameplay::GameplayTagRegistry& tags) {
    for (auto& [questId, progress] : log.quests) {
        if (progress.status != QuestStatus::Active) {
            continue;
        }
        progressTasks(forms, progress, event, tags);
        advanceCompletedBranch(forms, progress);
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
