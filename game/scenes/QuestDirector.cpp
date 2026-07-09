#include "game/scenes/QuestDirector.hpp"

#include "data/forms/CoreForms.hpp" // MiscItemForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormQuery.hpp"
#include "engine/core/Log.hpp"
#include "engine/ui/UiSystem.hpp"
#include "game/ScreenStack.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/actors/ActorState.hpp" // Bounty
#include "gameplay/event/EventBus.hpp"
#include "gameplay/inventory/Inventory.hpp"

namespace game {

void QuestDirector::beginScene(const QuestContext& ctx, bool loadedFromSave) {
    // Chantier 6 A2: gate tags registered up front so dialogue conditions
    // evaluate before any quest starts.
    questLog_ = quest::QuestLog {};
    easternQuest_ =
        data::findByEditorId<quest::QuestForm>(ctx.forms, "EasternMenace");
    for (const char* tag :
         { "Quest.EasternMenace.Active", "Quest.EasternMenace.Ready",
           "Quest.EasternMenace.Done", "Crime.Wanted" }) {
        ctx.gameTags.registerTag(tag);
    }
    // 8.7d: generic per-quest gate tags — dialogue conditions can gate on
    // Quest.<EditorId>.Active / .Done for ANY quest (modded included),
    // zero C++ per quest. (EasternMenace's .Ready turn-in window above
    // stays demo-wired until quests can express it in data.)
    data::forEach<quest::QuestForm>(
        ctx.forms, [&](const quest::QuestForm& quest) {
            if (quest.editorId.empty()) {
                return;
            }
            ctx.gameTags.registerTag("Quest." + quest.editorId + ".Active");
            ctx.gameTags.registerTag("Quest." + quest.editorId + ".Done");
        });
    // Chantier 6 A4: a loaded save rebuilds the quest log (the tag mirror
    // re-syncs after the player spawns, via syncQuestTags/syncWantedTag).
    if (loadedFromSave) {
        quest::applySavedQuests(questLog_, ctx.forms);
    }
}

void QuestDirector::reset() {
    dialogueRunner_.reset(); // references `forms`, reset before re-resolve
    dialoguePartner_ = ecs::Entity {};
    easternQuest_ = nullptr; // points into `forms`
    questLog_ = quest::QuestLog {};
}

void QuestDirector::syncQuestTags(const QuestContext& ctx) {
    if (!ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::AbilitySystem>()) {
        return;
    }
    auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
    const auto syncTag = [&](const str& name, bool want) {
        const auto tag = ctx.gameTags.find(name);
        if (!tag) {
            return;
        }
        const bool have = system.tags.has(*tag);
        if (want && !have) {
            system.tags.add(*tag, ctx.gameTags);
        } else if (!want && have) {
            system.tags.remove(*tag, ctx.gameTags);
        }
    };
    // 8.7d: every quest mirrors Active/Done generically (registered in
    // beginScene) — the gate tags any dialogue condition can use.
    data::forEach<quest::QuestForm>(
        ctx.forms, [&](const quest::QuestForm& quest) {
            if (quest.editorId.empty()) {
                return;
            }
            const bool taken = questLog_.quests.contains(quest.id);
            const bool active = quest::isActive(questLog_, quest.id);
            const bool done =
                taken && quest::questStatus(questLog_, quest.id) ==
                             quest::QuestStatus::Succeeded;
            syncTag("Quest." + quest.editorId + ".Active", active);
            syncTag("Quest." + quest.editorId + ".Done", done);
        });
    // EasternMenace's Ready (the turn-in window) stays demo-wired.
    if (easternQuest_) {
        const bool active = quest::isActive(questLog_, easternQuest_->id);
        const auto* reportState = data::findByEditorId<quest::QuestStateForm>(
            ctx.forms, "EasternMenaceReport");
        const bool ready =
            active && reportState &&
            quest::questState(questLog_, easternQuest_->id) ==
                reportState->id;
        syncTag("Quest.EasternMenace.Ready", ready);
    }
}

void QuestDirector::syncWantedTag(const QuestContext& ctx) {
    if (!ctx.playerEntity.is_alive()) {
        return;
    }
    const auto tag = ctx.gameTags.find("Crime.Wanted");
    if (!tag) {
        return;
    }
    const bool wanted =
        ctx.playerEntity.has<gameplay::Bounty>() &&
        ctx.playerEntity.get<gameplay::Bounty>().bounty > 0.0f;
    auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
    if (wanted) {
        system.tags.add(*tag, ctx.gameTags);
    } else {
        system.tags.remove(*tag, ctx.gameTags);
    }
}

void QuestDirector::handleQuestEvent(const QuestContext& ctx,
                                     const gameplay::Event& event) {
    // 8.7c — data-driven quest starts: any quest whose startEvent matches
    // begins now (never re-begins: the log keeps finished entries). The
    // old per-quest C++ subscription (acceptDemoQuest) is gone.
    const auto started =
        quest::startQuestsOn(questLog_, ctx.forms, event);
    for (const quest::QuestForm* quest : started) {
        syncQuestTags(ctx);
        ctx.say("Nouvelle quete : " +
                    (quest->displayName.empty() ? quest->editorId
                                                : quest->displayName) +
                    " (journal : J).",
                4.0f);
    }
    if (!easternQuest_) {
        return;
    }
    const core::Guid stateBefore =
        quest::questState(questLog_, easternQuest_->id);
    quest::onQuestEvent(questLog_, ctx.forms, event, ctx.gameTags);
    syncQuestTags(ctx);
    const bool succeeded = quest::questStatus(questLog_, easternQuest_->id) ==
                           quest::QuestStatus::Succeeded;
    if (succeeded && stateBefore.isValid() &&
        quest::questState(questLog_, easternQuest_->id) != stateBefore) {
        // The turn-in option fires exactly once (its gate tag drops with
        // the transition) — the reward lands here, no flag to persist.
        if (ctx.goldForm && ctx.playerEntity.is_alive() &&
            ctx.playerEntity.has<gameplay::Inventory>()) {
            gameplay::addItem(ctx.playerEntity.get_mut<gameplay::Inventory>(),
                              ctx.goldForm->id, 50);
        }
        ctx.say("Quete accomplie : La menace de l'est (+50 or).", 5.0f);
    } else if (quest::questState(questLog_, easternQuest_->id) != stateBefore) {
        ctx.say("Journal mis a jour (J).", 4.0f);
    }
}

void QuestDirector::payFine(const QuestContext& ctx) {
    // Chantier 6 D2: paying the fine clears the bounty. The option is gated
    // by HasTag Crime.Wanted + HasItem gold ≥ 40 in data, so the removeItem
    // below can only fail if a mod broke the gate — then it does nothing.
    if (!ctx.playerEntity.is_alive() || !ctx.goldForm) {
        return;
    }
    auto& bag = ctx.playerEntity.get_mut<gameplay::Inventory>();
    if (!gameplay::removeItem(bag, ctx.goldForm->id, 40)) {
        return;
    }
    if (ctx.playerEntity.has<gameplay::Bounty>()) {
        ctx.playerEntity.get_mut<gameplay::Bounty>().bounty = 0.0f;
    }
    syncWantedTag(ctx);
    ctx.say("Amende payee. Restez dans le droit chemin.", 4.0f);
}

void QuestDirector::openDialogue(const QuestContext& ctx,
                                 const core::Guid& dialogueId) {
    if (!ctx.uiCreated) {
        return;
    }
    if (!dialogueRunner_) {
        dialogueRunner_ =
            std::make_unique<quest::DialogueRunner>(ctx.forms, ctx.eventBus);
    }
    if (!dialogueRunner_->start(dialogueId)) {
        LOG_WARN("B4: dialogue {} failed to start", dialogueId.toString());
        return;
    }
    if (const auto* dialogue =
            ctx.forms.find<quest::DialogueForm>(dialogueId)) {
        ctx.ui.setString("dialogue", "npcName", dialogue->displayName);
    }
    ctx.pushDialogueModel();
    ctx.screenStack.show("dialogue");
}

} // namespace game
