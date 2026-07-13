#include "game/scenes/QuestDirector.hpp"

#include <unordered_set>

#include "data/forms/CoreForms.hpp" // MiscItemForm
#include "data/forms/FormDatabase.hpp"
#include "data/forms/LocForms.hpp" // TextTable (C9.5)
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
    std::unordered_set<core::Guid> startedIds;
    for (const quest::QuestForm* quest : started) {
        startedIds.insert(quest->id);
        syncQuestTags(ctx);
        // C9.5: every toast is a LocStringForm key — languages layer (§5).
        ctx.say(ctx.texts.format("quest.new",
                                 quest->displayName.empty()
                                     ? quest->editorId
                                     : quest->displayName),
                4.0f);
    }

    // 8.7e — generic progression: snapshot every quest, advance, then
    // toast state changes and pay DATA rewards (QuestForm.rewardItem/
    // rewardCount) on the first transition to Succeeded. The turn-in
    // option fires exactly once (its gate tag drops with the transition),
    // so the reward needs no persisted flag.
    std::unordered_map<core::Guid, std::pair<core::Guid, quest::QuestStatus>>
        before;
    for (const auto& [id, progress] : questLog_.quests) {
        before.emplace(id,
                       std::make_pair(progress.currentState, progress.status));
    }
    quest::onQuestEvent(questLog_, ctx.forms, event, ctx.gameTags);
    syncQuestTags(ctx);
    for (const auto& [id, progress] : questLog_.quests) {
        if (startedIds.contains(id)) {
            continue; // the "Nouvelle quete" toast already covered it
        }
        const auto it = before.find(id);
        if (it == before.end()) {
            continue;
        }
        const quest::QuestForm* quest = ctx.forms.find<quest::QuestForm>(id);
        if (!quest) {
            continue;
        }
        const str name =
            quest->displayName.empty() ? quest->editorId : quest->displayName;
        const bool nowSucceeded =
            progress.status == quest::QuestStatus::Succeeded &&
            it->second.second != quest::QuestStatus::Succeeded;
        if (nowSucceeded) {
            str rewardText;
            if (quest->rewardItem.isValid() && quest->rewardCount > 0 &&
                ctx.playerEntity.is_alive() &&
                ctx.playerEntity.has<gameplay::Inventory>()) {
                gameplay::addItem(
                    ctx.playerEntity.get_mut<gameplay::Inventory>(),
                    quest->rewardItem, quest->rewardCount);
                // The item's displayName when it has one (reflection —
                // item categories differ), else its editorId.
                str itemName;
                const data::FormHandle handle =
                    ctx.forms.handleOf(quest->rewardItem);
                if (handle.isValid()) {
                    const data::Form* item = ctx.forms.get(handle);
                    itemName = item->editorId;
                    if (const auto* itemType = ctx.forms.typeOf(handle)) {
                        if (const auto* field = itemType->findField(
                                core::fnv1a("displayName"))) {
                            const reflect::Value value = field->get(item);
                            if (const str* display = std::get_if<str>(&value);
                                display && !display->empty()) {
                                itemName = *display;
                            }
                        }
                    }
                }
                rewardText = ctx.texts.format(
                    "quest.reward",
                    { std::to_string(quest->rewardCount), itemName });
            }
            ctx.say(ctx.texts.format("quest.done", { name, rewardText }),
                    5.0f);
        } else if (progress.status == quest::QuestStatus::Failed &&
                   it->second.second != quest::QuestStatus::Failed) {
            // 8.7e follow-up: failing through a Failure state announces
            // itself (no reward, obviously).
            ctx.say(ctx.texts.format("quest.failed", name), 5.0f);
        } else if (progress.currentState != it->second.first) {
            ctx.say(ctx.texts.get("quest.journalUpdated"), 4.0f);
        }
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
        // Per-faction crime (2026-07-13): the fine settles the ARRESTING
        // guard's faction (the dialogue partner) + the unattributed part;
        // another faction's slice survives — its own guards stay hostile
        // and the Wanted mirror below keeps the tag while any remains.
        gameplay::clearBountyToward(
            ctx.playerEntity.get_mut<gameplay::Bounty>(),
            ctx.factionOf ? ctx.factionOf(dialoguePartner_)
                          : gameplay::GameplayTag {});
    }
    syncWantedTag(ctx);
    ctx.say(ctx.texts.get("crime.finePaid"), 4.0f);
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
    // 8.7e: the node's world side effects — takeItem/takeCount removes
    // from the player when the node fires ("here are the rations"). Set
    // per open with the CURRENT player entity; gate the option with a
    // HasItem condition in data, the removal itself does not re-check.
    dialogueRunner_->onNodeFired =
        [player = ctx.playerEntity](const quest::DialogueNodeForm& node) {
            if (!node.takeItem.isValid() || node.takeCount <= 0 ||
                !player.is_alive() || !player.has<gameplay::Inventory>()) {
                return;
            }
            auto& bag = player.get_mut<gameplay::Inventory>();
            if (!gameplay::removeItem(bag, node.takeItem, node.takeCount)) {
                LOG_WARN("Dialogue takeItem: player lacks {} x{} — gate the "
                         "option with a HasItem condition",
                         node.takeItem.toString(), node.takeCount);
            }
        };
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
