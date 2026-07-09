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
    if (!easternQuest_ || !ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::AbilitySystem>()) {
        return;
    }
    auto& system = ctx.playerEntity.get_mut<gameplay::AbilitySystem>();
    const auto syncTag = [&](const char* name, bool want) {
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
    const bool active = quest::isActive(questLog_, easternQuest_->id);
    const auto* reportState = data::findByEditorId<quest::QuestStateForm>(
        ctx.forms, "EasternMenaceReport");
    const bool ready =
        active && reportState &&
        quest::questState(questLog_, easternQuest_->id) == reportState->id;
    const bool done = quest::questStatus(questLog_, easternQuest_->id) ==
                      quest::QuestStatus::Succeeded;
    syncTag("Quest.EasternMenace.Active", active);
    syncTag("Quest.EasternMenace.Ready", ready);
    syncTag("Quest.EasternMenace.Done", done);
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

void QuestDirector::acceptDemoQuest(const QuestContext& ctx) {
    if (!easternQuest_ || questLog_.quests.contains(easternQuest_->id)) {
        return; // already taken (or done) — never re-begin
    }
    quest::beginQuest(questLog_, ctx.forms, easternQuest_->id);
    syncQuestTags(ctx);
    ctx.say("Nouvelle quete : La menace de l'est (journal : J).", 4.0f);
}

void QuestDirector::handleQuestEvent(const QuestContext& ctx,
                                     const gameplay::Event& event) {
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
