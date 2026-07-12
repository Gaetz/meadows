#include "game/scenes/UiRouter.hpp"

#include <cstdio>
#include <ctime>
#include <optional>

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp" // ActorForm, WeaponForm, ArmorForm...
#include "data/forms/FormDatabase.hpp"
#include "engine/core/Log.hpp"
#include "engine/ui/UiSystem.hpp"
#include "game/Barter.hpp"
#include "game/SaveGame.hpp" // listSaveSlots
#include "game/ScreenStack.hpp"
#include "game/scenes/GameHud.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/actors/ActorState.hpp"     // gameplay::VendorState
#include "gameplay/actors/CharacterForms.hpp" // gameplay::applyLoadout
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/Survival.hpp"
#include "quest/Dialogue.hpp"
#include "world/scene/Components.hpp"

namespace game {

// --- Chantier 4 B3: inventory / container --------------------------------------------

void UiRouter::openInventoryScreen(const UiRouterContext& ctx) {
    containerEntity_ = ecs::Entity {};
    barterMode_ = false;
    ctx.ui.setBool("inventory", "transferMode", false);
    ctx.pushItemModels();
    ctx.screenStack.show("inventory");
}

void UiRouter::openContainerScreen(const UiRouterContext& ctx,
                                   ecs::Entity container) {
    containerEntity_ = container;
    barterMode_ = false;
    if (containerEntity_.is_alive() &&
        !containerEntity_.has<gameplay::Inventory>()) {
        containerEntity_.set<gameplay::Inventory>({});
    }
    ctx.ui.setBool("inventory", "transferMode", true);
    ctx.pushItemModels();
    ctx.screenStack.show("container");
}

void UiRouter::openBarterScreen(const UiRouterContext& ctx,
                                ecs::Entity vendor) {
    if (!vendor.is_alive() || !ctx.goldForm ||
        !ctx.playerEntity.is_alive()) {
        return;
    }
    containerEntity_ = vendor;
    barterMode_ = true;
    if (!containerEntity_.has<gameplay::Inventory>()) {
        containerEntity_.set<gameplay::Inventory>({});
    }
    ctx.ui.setBool("inventory", "transferMode", true);
    // The vendor's name for the title + its barter profile (D1).
    str title = "Merchant";
    vendorBuyMult_ = ctx.statsTuning.barterBuyMult;
    vendorSellMult_ = ctx.statsTuning.barterSellMult;
    core::Guid vendorFormId;
    if (containerEntity_.has<world::RefId>()) {
        const auto& ref = containerEntity_.get<world::RefId>();
        if (const reflect::TypeInfo* type = ctx.forms.typeOf(ref.base);
            type && type->isA(data::ActorForm::staticTypeInfo().id)) {
            const auto* actor =
                static_cast<const data::ActorForm*>(ctx.forms.get(ref.base));
            if (!actor->displayName.empty()) {
                title = actor->displayName;
            }
            if (actor->buyMult > 0.0f) {
                vendorBuyMult_ = actor->buyMult;
            }
            if (actor->sellMult > 0.0f) {
                vendorSellMult_ = actor->sellMult;
            }
            vendorFormId = actor->id;
        }
    }
    ctx.ui.setString("barter", "title", title);

    // D1: restock — more than kRestockHours of game time since the last
    // re-roll = clear + a fresh loadout roll (gold re-rolls with it, the
    // Skyrim behavior). VendorState is a reflected component so the save
    // layer carries the clock (a scene map would reset on re-enter = a
    // free-restock exploit).
    const f64 kRestockHours =
        static_cast<f64>(ctx.statsTuning.vendorRestockHours); // U4-7
    const f64 nowHours = ctx.gameClock.gameHours();
    if (!containerEntity_.has<gameplay::VendorState>()) {
        containerEntity_.set<gameplay::VendorState>({});
    }
    auto& vendorState = containerEntity_.get_mut<gameplay::VendorState>();
    if (vendorState.lastRestockHours <= 0.0f) {
        // First open: stamp the clock, the spawn loadout IS the stock.
        vendorState.lastRestockHours = static_cast<f32>(nowHours);
    } else if (vendorFormId.isValid() &&
               nowHours - vendorState.lastRestockHours > kRestockHours) {
        auto& stock = containerEntity_.get_mut<gameplay::Inventory>();
        stock.items.clear();
        gameplay::applyLoadout(ctx.forms, vendorFormId, stock, ctx.lootRng);
        vendorState.lastRestockHours = static_cast<f32>(nowHours);
        LOG_INFO("Vendor restocked ({}h game time)", nowHours);
    }
    ctx.pushItemModels();
    ctx.screenStack.show("barter");
}

void UiRouter::barterTrade(const UiRouterContext& ctx, const core::Guid& item,
                           bool playerBuys) {
    if (!barterMode_ || !ctx.goldForm || !containerEntity_.is_alive() ||
        !ctx.playerEntity.is_alive()) {
        return;
    }
    // The unit value comes from the view row (already resolved per kind).
    const InventoryView& side =
        playerBuys ? ctx.hud.loot() : ctx.hud.inventory();
    const InventoryView::Row* row = nullptr;
    for (const InventoryView::Row& candidate : side.rows()) {
        if (candidate.id == item) {
            row = &candidate;
            break;
        }
    }
    if (!row) {
        return;
    }
    auto& bag = ctx.playerEntity.get_mut<gameplay::Inventory>();
    auto& stock = containerEntity_.get_mut<gameplay::Inventory>();
    if (playerBuys) {
        const i32 price = barterPrice(row->value, vendorBuyMult_);
        barterBuy(bag, stock, item, price, ctx.goldForm->id);
    } else {
        const i32 price = barterPrice(row->value, vendorSellMult_);
        barterSell(bag, stock, item, price, ctx.goldForm->id);
    }
}

void UiRouter::transferItem(const UiRouterContext& ctx, const core::Guid& id,
                            bool fromContainer) {
    if (!id.isValid() || !containerEntity_.is_alive() ||
        !ctx.playerEntity.is_alive()) {
        return;
    }
    if (!containerEntity_.has<gameplay::Inventory>() ||
        !ctx.playerEntity.has<gameplay::Inventory>()) {
        return;
    }
    auto& loot = containerEntity_.get_mut<gameplay::Inventory>();
    auto& bag = ctx.playerEntity.get_mut<gameplay::Inventory>();
    auto& source = fromContainer ? loot : bag;
    auto& target = fromContainer ? bag : loot;
    if (gameplay::removeItem(source, id, 1)) {
        gameplay::addItem(target, id, 1);
    }
}

void UiRouter::toggleEquip(const UiRouterContext& ctx, const core::Guid& id) {
    if (!id.isValid() || !ctx.playerEntity.is_alive() ||
        !ctx.playerEntity.has<gameplay::Equipment>()) {
        return;
    }
    auto& equipment = ctx.playerEntity.get_mut<gameplay::Equipment>();
    const data::FormHandle handle = ctx.forms.handleOf(id);
    const reflect::TypeInfo* type = ctx.forms.typeOf(handle);
    if (!type) {
        return;
    }
    if (type->isA(data::WeaponForm::staticTypeInfo().id)) {
        equipment.weapon = equipment.weapon == id ? core::Guid {} : id;
    } else if (type->isA(data::ArmorForm::staticTypeInfo().id)) {
        const auto* armor =
            static_cast<const data::ArmorForm*>(ctx.forms.get(handle));
        core::Guid* slot = nullptr;
        if (armor->slot == "head") {
            slot = &equipment.head;
        } else if (armor->slot == "torso") {
            slot = &equipment.torso;
        } else if (armor->slot == "arms") {
            slot = &equipment.arms;
        } else if (armor->slot == "legs") {
            slot = &equipment.legs;
        }
        if (slot) {
            *slot = *slot == id ? core::Guid {} : id;
        }
    }
}

void UiRouter::useConsumable(const UiRouterContext& ctx,
                             const core::Guid& id) {
    if (!id.isValid() || !ctx.playerEntity.is_alive()) {
        return;
    }
    const auto* consumable = ctx.forms.find<data::ConsumableForm>(id);
    if (!consumable || !ctx.playerEntity.has<gameplay::Inventory>()) {
        return;
    }
    auto& bag = ctx.playerEntity.get_mut<gameplay::Inventory>();
    if (!gameplay::removeItem(bag, id, 1)) {
        return;
    }
    // Survival needs are component fields (the sleep() precedent);
    // attribute changes still go through effects only (§2.9).
    if (ctx.playerEntity.has<gameplay::Survival>()) {
        auto& survival = ctx.playerEntity.get_mut<gameplay::Survival>();
        survival.hunger = glm::min(100.0f, survival.hunger +
                                               consumable->restoreHunger);
        survival.thirst = glm::min(100.0f, survival.thirst +
                                               consumable->restoreThirst);
    }
    if (consumable->effect.isValid()) {
        if (const auto* effect =
                ctx.forms.find<gameplay::EffectForm>(consumable->effect)) {
            gameplay::applyEffect(
                ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
                ctx.playerEntity.get_mut<gameplay::AbilitySystem>(), *effect,
                ctx.gameTags);
        }
    }
    LOG_INFO("Used: {}", consumable->editorId);
}

// --- The data-event dispatch ----------------------------------------------------------

void UiRouter::handleUiEvent(const UiRouterContext& ctx, const str& model,
                             const str& event, const vector<str>& args) {
    const auto argGuid = [&]() -> std::optional<core::Guid> {
        return args.empty() ? std::nullopt
                            : core::Guid::fromString(args[0]);
    };
    if (model == "inventory") {
        if (event == "tab" && !args.empty()) {
            using Category = InventoryView::Category;
            Category category = Category::All;
            if (args[0] == "weapons") {
                category = Category::Weapons;
            } else if (args[0] == "armor") {
                category = Category::Armor;
            } else if (args[0] == "consumables") {
                category = Category::Consumables;
            } else if (args[0] == "misc") {
                category = Category::Misc;
            }
            ctx.hud.inventory().setCategory(category);
        } else if (event == "sortCol" && !args.empty()) {
            using Column = InventoryView::Column;
            Column column = Column::Name;
            if (args[0] == "weight") {
                column = Column::Weight;
            } else if (args[0] == "value") {
                column = Column::Value;
            } else if (args[0] == "power") {
                column = Column::Power;
            }
            ctx.hud.inventory().sortBy(column);
        } else if (event == "pick") {
            if (const auto id = argGuid()) {
                if (barterMode_) {
                    barterTrade(ctx, *id, /*playerBuys=*/false); // sell
                } else if (containerEntity_.is_alive()) {
                    transferItem(ctx, *id, /*fromContainer=*/false);
                } else {
                    ctx.hud.inventory().select(*id);
                }
            }
        } else if (event == "equipAction") {
            toggleEquip(ctx, ctx.hud.inventory().selected());
        } else if (event == "useAction") {
            useConsumable(ctx, ctx.hud.inventory().selected());
        }
        ctx.pushItemModels();
    } else if (model == "container") {
        if (event == "pickLoot") {
            if (const auto id = argGuid()) {
                transferItem(ctx, *id, /*fromContainer=*/true);
            }
        } else if (event == "takeAll" && containerEntity_.is_alive() &&
                   ctx.playerEntity.is_alive()) {
            auto& loot = containerEntity_.get_mut<gameplay::Inventory>();
            auto& bag = ctx.playerEntity.get_mut<gameplay::Inventory>();
            for (const gameplay::ItemStack& stack : loot.items) {
                if (stack.count > 0) {
                    gameplay::addItem(bag, stack.item, stack.count);
                }
            }
            loot.items.clear();
        }
        ctx.pushItemModels();
    } else if (model == "barter") {
        if (event == "pickBuy") {
            if (const auto id = argGuid()) {
                barterTrade(ctx, *id, /*playerBuys=*/true);
            }
        }
        ctx.pushItemModels();
    } else if (model == "menu") {
        if (event == "menuAction" && !args.empty()) {
            handleMenuAction(ctx, args[0]);
        }
    } else if (model == "saves") {
        if (event == "loadSlot" && !args.empty()) {
            ctx.screenStack.closeAll();
            ctx.requestLoad(args[0]);
        } else if (event == "loadCancel") {
            ctx.screenStack.closeTop();
        }
    } else if (model == "journal") {
        if (event == "journalClose") {
            ctx.screenStack.closeTop();
        }
    } else if (model == "dialogue") {
        if (event == "choose" && !args.empty() && ctx.dialogueRunner) {
            if (args[0] == "leave") {
                ctx.dialogueRunner->end();
            } else if (const auto id = core::Guid::fromString(args[0])) {
                for (const quest::DialogueNodeForm* option :
                     ctx.hud.dialogueOptions()) {
                    if (option->id == *id) {
                        ctx.dialogueRunner->select(*option);
                        break;
                    }
                }
            }
            // Closes the screen when the dialogue ended.
            ctx.pushDialogueModel();
        }
    }
}

// --- Chantier 4 B6: menus --------------------------------------------------------------

void UiRouter::handleMenuAction(const UiRouterContext& ctx,
                                const str& action) {
    if (action == "resume" || action == "cancel") {
        ctx.screenStack.closeTop();
    } else if (action == "save") {
        // Timestamped manual slot; F5 owns "quick".
        char slot[32];
        const std::time_t now = std::time(nullptr);
        std::tm local {};
        if (localtime_s(&local, &now) == 0) {
            std::strftime(slot, sizeof(slot), "save_%Y%m%d_%H%M%S", &local);
        } else {
            std::snprintf(slot, sizeof(slot), "save_manual");
        }
        ctx.performSave(slot);
        ctx.screenStack.closeTop();
    } else if (action == "loadmenu") {
        vector<::ui::UiRow> rows;
        for (const SaveSlotInfo& info : listSaveSlots()) {
            ::ui::UiRow row;
            row.id = info.name;
            row.c0 = info.name;
            row.c1 = info.timestamp;
            rows.push_back(std::move(row));
        }
        ctx.ui.setBool("saves", "empty", rows.empty());
        ctx.ui.setRows("saves", std::move(rows));
        ctx.screenStack.show("saves");
    } else if (action == "wait") {
        ctx.updateMenuClockLine();
        ctx.screenStack.show("wait");
    } else if (action == "wait1" || action == "wait4" ||
               action == "wait8") {
        ctx.wait(action == "wait1" ? 1.0f
                                   : action == "wait4" ? 4.0f : 8.0f);
        ctx.updateMenuClockLine();
        ctx.screenStack.closeTop();
    } else if (action == "play") {
        ctx.screenStack.closeAll();
        if (!ctx.playMode) {
            ctx.enterPlayMode();
        }
    } else if (action == "mainmenu") {
        if (ctx.playMode) {
            ctx.exitPlayMode();
        }
        ctx.screenStack.closeAll();
        ctx.updateMenuClockLine();
        ctx.screenStack.show("mainmenu");
    } else if (action == "options") {
        // C9.4: the scene fills the "options" model (settings + bindings
        // are its state) and shows the screen over the current menu.
        ctx.openOptions();
    } else if (action == "quit") {
        ctx.requestQuit();
    }
}

void UiRouter::reset() {
    containerEntity_ = ecs::Entity {};
    barterMode_ = false;
    vendorBuyMult_ = 1.5f;
    vendorSellMult_ = 0.5f;
}

} // namespace game
