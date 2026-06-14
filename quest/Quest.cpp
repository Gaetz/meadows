#include "quest/Quest.hpp"

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "gameplay/ability/GameplayTags.hpp"

namespace quest {

namespace {

template<typename T, typename Fn>
void forEachForm(const data::FormDatabase& forms, Fn&& fn) {
    const u32 typeId = T::staticTypeInfo().id;
    for (u32 value = 1; value <= forms.count(); ++value) {
        const data::FormHandle handle { value };
        const reflect::TypeInfo* type = forms.typeOf(handle);
        const data::Form* form = forms.get(handle);
        if (type && form && type->isA(typeId)) {
            fn(*static_cast<const T*>(form));
        }
    }
}

// Is a branch complete (all its tasks at or past `required`, and it has tasks)?
bool branchComplete(const data::FormDatabase& forms, const core::Guid& branchId,
                    const QuestProgress& progress) {
    bool hasTask = false;
    bool allDone = true;
    forEachForm<QuestTaskForm>(forms, [&](const QuestTaskForm& task) {
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
    forEachForm<QuestBranchForm>(forms, [&](const QuestBranchForm& branch) {
        if (branch.state != progress.currentState) {
            return;
        }
        forEachForm<QuestTaskForm>(forms, [&](const QuestTaskForm& task) {
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
    forEachForm<QuestBranchForm>(forms, [&](const QuestBranchForm& branch) {
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

} // namespace quest
