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
// from LandscapeScene (audit U4-1). The scene rebuilds it per dispatch —
// references plus the scene actions that stay its territory as closures
// (saves, mode flips, waiting, the model pushes that need a HudContext).
struct UiRouterContext {
    data::FormDatabase& forms;
    const data::TextTable& texts; // C9.5: ui.* fallback strings
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
    std::function<void()> requestQuit;
    std::function<void()> openOptions; // C9.4: push model + show screen
    // FOLLOWERS É7 (appended): the HUD toast — the transfer guards talk
    // back (refused base-kit item, overweight follower, auto-equip).
    std::function<void(str line)> say;
};

// The UI action router extracted from LandscapeScene (audit U4-1): the
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
    // The RmlUi data-event callback target (model, event, args).
    void handleUiEvent(const UiRouterContext& ctx, const str& model,
                       const str& event, const vector<str>& args);

    // Pause / main / wait / workstation menus share one action vocabulary.
    void handleMenuAction(const UiRouterContext& ctx, const str& action);

    // Item screens: player-only, loot, and the B5 barter screen.
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
