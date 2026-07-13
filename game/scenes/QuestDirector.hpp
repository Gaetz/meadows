#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity
#include "gameplay/ability/GameplayTags.hpp" // QuestContext.factionOf
#include "quest/Dialogue.hpp"   // DialogueRunner (held by uptr — full type)
#include "quest/Quest.hpp"      // QuestLog (value member)

namespace ui {
class UiSystem;
}
namespace data {
class FormDatabase;
struct MiscItemForm;
class TextTable;
}
namespace gameplay {
class GameplayTagRegistry;
class EventBus;
struct Event;
}

namespace game {

class ScreenStack;

// The scene systems the quest / crime / dialogue logic touches, bundled so
// that logic is decoupled from LandscapeScene (audit U4-1). Rebuilt per call
// (cheap) — references plus the two scene actions the director needs as
// closures (the toast, the dialogue model push that needs a HudContext). The
// eventBus stays a SCENE hub (dialogue and combat both publish into it); the
// director only reads/subscribes through it.
struct QuestContext {
    data::FormDatabase& forms;
    const data::TextTable& texts;            // C9.5: quest.* toast strings
    gameplay::GameplayTagRegistry& gameTags; // registerTag mutates
    gameplay::EventBus& eventBus;            // DialogueRunner publishes here
    ::ui::UiSystem& ui;
    ScreenStack& screenStack;
    ecs::Entity playerEntity;
    const data::MiscItemForm* goldForm;
    bool uiCreated;
    std::function<void(const str&, f32)> say; // interaction.say
    std::function<void()> pushDialogueModel;  // hud.pushDialogueModel(HudCtx)
    // Per-faction crime (2026-07-13): resolves an actor entity to its
    // faction tag (the scene's Npc registry) — payFine settles the
    // ARRESTING guard's faction. Null/invalid = clear the whole bounty.
    std::function<gameplay::GameplayTag(ecs::Entity)> factionOf;
};

// The quest / crime / dialogue director extracted from LandscapeScene (audit
// U4-1): the demo quest state machine (the EasternMenace tree), its mirror
// into player gate tags (dialogue conditions can't read components), the
// crime bounty → Crime.Wanted mirror, and the dialogue runner. It owns the
// quest log, the demo-quest pointer, the dialogue runner and the [E]-Talk
// partner; the scene reaches the shared bits (log for save/HUD/console,
// runner for the HUD, partner for barter) through the accessors. Event
// SUBSCRIPTIONS stay in the scene's onEnter (its `this` is stable for the
// eventBus lifetime) and delegate to acceptDemoQuest / handleQuestEvent /
// payFine with a fresh context.
class QuestDirector {
public:
    // onEnter: reset the log, register the demo quest's gate tags, and
    // rebuild the saved log when loading. The tag mirror re-syncs after the
    // player spawns (syncQuestTags/syncWantedTag).
    void beginScene(const QuestContext& ctx, bool loadedFromSave);
    // onExit: drop the runner + demo-quest pointer + log (all point into
    // `forms`, reset before re-resolve).
    void reset();

    // Quest / crime state mirrored into PLAYER tags.
    void syncQuestTags(const QuestContext& ctx);
    void syncWantedTag(const QuestContext& ctx);

    // Event bodies — the scene's subscriptions call these. Since 8.7c the
    // scene subscribes ONCE for everything (subscribeAll): quest starts
    // (QuestForm.startEvent) and task progression are data vocabularies.
    void handleQuestEvent(const QuestContext& ctx, const gameplay::Event& event);
    void payFine(const QuestContext& ctx);

    // Open a dialogue tree (lazily builds the runner; surfaces the RmlUi
    // screen through the context).
    void openDialogue(const QuestContext& ctx, const core::Guid& dialogueId);

    // Shared bits other systems reach: the log (save / HUD / console), the
    // runner (HUD / UiRouter), the [E]-Talk partner (barter opens on it).
    quest::QuestLog& questLog() { return questLog_; }
    const quest::QuestLog& questLog() const { return questLog_; }
    quest::DialogueRunner* dialogueRunner() { return dialogueRunner_.get(); }
    ecs::Entity dialoguePartner() const { return dialoguePartner_; }
    void setDialoguePartner(ecs::Entity partner) { dialoguePartner_ = partner; }

private:
    quest::QuestLog questLog_;
    const quest::QuestForm* easternQuest_ { nullptr };
    uptr<quest::DialogueRunner> dialogueRunner_;
    ecs::Entity dialoguePartner_ {}; // who [E] Talk opened (vendor for B5)
};

} // namespace game
