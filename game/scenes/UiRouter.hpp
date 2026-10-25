#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/core/Rng.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity

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
struct StatsTuningForm;
struct GameClock;
}
namespace quest {
class DialogueRunner;
}

namespace game {

class GameHud;
class ScreenStack;

// The scene systems the UI ACTION routing touches, bundled so the
// data-event dispatch (handleUiEvent), the menu actions, the item screens'
// opening logic and the inventory/barter gameplay actions are decoupled
// from LandscapeScene. The scene rebuilds it per dispatch —
// references plus the scene actions that stay its territory as closures
// (saves, mode flips, waiting, the model pushes that need a HudContext).
struct UiRouterContext {
    data::FormDatabase& forms;
    const data::TextTable& texts; // Ui.* fallback strings
    ::ui::UiSystem& ui;
    ScreenStack& screenStack;
    GameHud& hud; // view-model state (inventory()/loot()/dialogueOptions())
    const gameplay::GameplayTagRegistry& gameTags;
    const gameplay::StatsTuningForm& statsTuning;
    const gameplay::GameClock& gameClock; // vendor restock stamp
    core::Rng& lootRng;                   // restock loadout rolls (§8 seeded)
    const data::MiscItemForm* goldForm;
    quest::DialogueRunner* dialogueRunner; // choose/leave routing
    ecs::Entity playerEntity;
    bool playMode; // menu "play"/"mainmenu" gating
    // Scene actions (closures over scene state):
    std::function<void()> pushItemModels;      // hud + HudContext
    std::function<void()> pushDialogueModel;   // idem (closes when ended)
    std::function<void()> updateMenuClockLine; // idem
    std::function<void(const str& slot)> performSave;
    std::function<void(const str& slot)> requestLoad;
    std::function<void(f32 hours)> wait; // interaction.wait + clock line
    std::function<void()> enterPlayMode;
    std::function<void()> exitPlayMode;
    // Main-menu world pick: story (the authored world) or sandbox (the
    // bounded generated map). The scene's closure owns the whole game
    // entry (world flipped, menus closed, Play entered) — one body shared
    // with the boot override. Sits between exitPlayMode and requestQuit
    // in the initializer.
    std::function<void(bool sandbox)> beginPlay;
    std::function<void()> requestQuit;
    std::function<void()> openOptions; // Push model + show screen
    // The HUD toast — the transfer guards talk
    // back (refused base-kit item, overweight follower, auto-equip).
    std::function<void(str line)> say;
};

// The UI action router extracted from LandscapeScene: the
// RmlUi data-event dispatch (the setModelEventHandler callback lands here),
// the shared menu actions, the inventory/container/barter screen opening
// (incl. the D1 vendor profile + 24h restock) and the item gameplay actions
// (equip/use/transfer/trade). It owns the routing STATE the screens share:
// the open container/vendor, the barter flag and the effective vendor
// multipliers — GameHud reads them through the accessors when it prices
// the item tables. Dialogue OPENING stays in the scene (quest territory);
// this only routes the choose/leave clicks back into the runner.
class UiRouter {
public:
    // The RmlUi data-event callback target (model, event, args). Model and
    // event names parse through their tables; a (model, event) pair no
    // route accepts is logged once instead of being dropped silently.
    void handleUiEvent(const UiRouterContext& ctx, const str& model,
                       const str& event, const vector<str>& args);

    // Pause / main / wait / workstation menus share one action vocabulary;
    // "wait<N>" carries its duration (hours) in the action name.
    void handleMenuAction(const UiRouterContext& ctx, const str& action);

    // Item screens: player-only, loot, and the barter screen.
    void openInventoryScreen(const UiRouterContext& ctx);
    void openContainerScreen(const UiRouterContext& ctx, ecs::Entity container);
    void openBarterScreen(const UiRouterContext& ctx, ecs::Entity vendor);

    // GameHud reads these when pricing/building the item tables.
    ecs::Entity containerEntity() const { return containerEntity_; }
    bool barterMode() const { return barterMode_; }
    f32 vendorBuyMult() const { return vendorBuyMult_; }
    f32 vendorSellMult() const { return vendorSellMult_; }

    // onExit teardown: forget the open container/vendor (entities die with
    // the world) and reset the barter state.
    void reset();

private:
    // The routed data-event handlers, one per (model, event) row of the
    // route table in handleUiEvent.
    void onInventoryTab(const UiRouterContext& ctx, const vector<str>& args);
    void onInventorySortCol(const UiRouterContext& ctx,
                            const vector<str>& args);
    void onInventoryPick(const UiRouterContext& ctx, const vector<str>& args);
    void onInventoryEquip(const UiRouterContext& ctx, const vector<str>& args);
    void onInventoryUse(const UiRouterContext& ctx, const vector<str>& args);
    void onContainerPickLoot(const UiRouterContext& ctx,
                             const vector<str>& args);
    void onContainerTakeAll(const UiRouterContext& ctx,
                            const vector<str>& args);
    void onBarterPickBuy(const UiRouterContext& ctx, const vector<str>& args);
    void onMenuAction(const UiRouterContext& ctx, const vector<str>& args);
    void onSavesLoadSlot(const UiRouterContext& ctx, const vector<str>& args);
    void onSavesLoadCancel(const UiRouterContext& ctx,
                           const vector<str>& args);
    void onJournalClose(const UiRouterContext& ctx, const vector<str>& args);
    void onDialogueChoose(const UiRouterContext& ctx, const vector<str>& args);

    void barterTrade(const UiRouterContext& ctx, const core::Guid& item,
                     bool playerBuys);
    void toggleEquip(const UiRouterContext& ctx, const core::Guid& id);
    void useConsumable(const UiRouterContext& ctx, const core::Guid& id);
    void transferItem(const UiRouterContext& ctx, const core::Guid& id,
                      bool fromContainer);

    ecs::Entity containerEntity_ {};
    bool barterMode_ { false };
    // D1: the OPEN vendor's effective multipliers (ActorForm override or
    // the global tuning), captured by openBarterScreen.
    f32 vendorBuyMult_ { 1.5f };
    f32 vendorSellMult_ { 0.5f };
};

} // namespace game
