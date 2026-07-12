#pragma once

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity
#include "game/InventoryView.hpp"
#include "gameplay/condition/Condition.hpp" // gameplay::EvalContext (by value)

namespace ui {
class UiSystem;
}
namespace render {
class FlyCamera;
}
namespace data {
class FormDatabase;
struct MiscItemForm;
class TextTable;
}
namespace phys {
class CharacterBody;
}
namespace gameplay {
struct GameClock;
}
namespace quest {
struct QuestLog;
class DialogueRunner;
struct DialogueNodeForm;
}

namespace game {

struct Npc;
class ScreenStack;
class InteractionController;

// The scene systems the RmlUi view-model pushes read, bundled so the
// presentation layer (game state -> UiSystem writes) is decoupled from
// LandscapeScene (audit U4-9). The scene rebuilds it each call from its own
// members — references plus a few scalars. Mirrors the other *Context
// contracts (Editor/Streaming/Npc/Interaction).
struct HudContext {
    ::ui::UiSystem& ui;
    bool uiCreated;
    data::FormDatabase& forms;
    const data::TextTable& texts; // C9.5: ui.* strings the C++ side formats
    ecs::Entity playerEntity;
    const gameplay::GameClock& gameClock;
    const InteractionController& interaction; // prompt/talk slots
    bool playMode;                            // mode == Play
    // Nameplates: world -> screen projection.
    const render::FlyCamera& flyCamera;
    f32 screenWidth;
    f32 screenHeight;
    phys::CharacterBody* player;
    const vector<uptr<Npc>>& npcs;
    // Item screens (inventory / container / barter):
    ecs::Entity containerEntity;
    bool barterMode;
    f32 vendorBuyMult;
    f32 vendorSellMult;
    const data::MiscItemForm* goldForm;
    // Journal + dialogue:
    const quest::QuestLog& questLog;
    quest::DialogueRunner* dialogueRunner;
    gameplay::EvalContext evalContext; // scene-built (player tags/bag); by
                                       //   value — pointers, cheap, no dangling
    ScreenStack& screenStack; // pushDialogueModel closes "dialogue" when done
    // A7+: the bow-draw gauge (0..1 while drawing, < 0 = hidden).
    f32 bowCharge;
    // R7: vitals-bar scale — stat points per 1% of the bar container
    // (StatsTuningForm.hudStatPointsScale, filled by the scene).
    f32 hudStatPointsScale;
};

// The RmlUi presenter extracted from LandscapeScene (audit U4-9): every
// push*Model / update*Model that translates game state into UiSystem data
// models lives here, along with the view-model state it feeds (the two
// InventoryViews and the dialogue option list). Game ACTIONS stay in the
// scene (handleUiEvent/handleMenuAction routing, equip/use/transfer/barter,
// open*Screen orchestration) — the scene mutates the views through the
// accessors when an event demands it, the NpcDirector::npcs() pattern.
class GameHud {
public:
    // Per frame: vitals/clock/prompt/talk slots + the NPC nameplates.
    void updateHudModel(const HudContext& ctx);

    // Item screens: rebuild the player table (and the loot/barter side when
    // a container is open) into the inventory/container/barter models.
    void pushItemModels(const HudContext& ctx);

    // Quest journal rows (deterministic: quests sorted by guid, §8).
    void pushJournalModel(const HudContext& ctx);

    // Dialogue line + options; closes the screen when the runner ended.
    void pushDialogueModel(const HudContext& ctx);

    // The pause/main/wait menus' shared "Day N — HH:MM" line.
    void updateMenuClockLine(const HudContext& ctx);

    // Event routing (scene) reads/mutates the view state directly.
    InventoryView& inventory() { return invView; }
    const InventoryView& inventory() const { return invView; }
    const InventoryView& loot() const { return lootView; }
    const vector<const quest::DialogueNodeForm*>& dialogueOptions() const {
        return dialogueOptions_;
    }

    // onExit teardown: the options point into `forms` — drop before
    // re-resolve; views reset so a re-enter starts clean.
    void reset();

private:
    void updateNameplates(const HudContext& ctx);

    InventoryView invView;
    InventoryView lootView;
    vector<const quest::DialogueNodeForm*> dialogueOptions_;
};

} // namespace game
