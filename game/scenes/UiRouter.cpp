#include "game/scenes/UiRouter.hpp"

#include <cstdio>
#include <ctime>
#include <optional>

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp" // ActorForm, WeaponForm, ArmorForm...
#include "data/forms/FormDatabase.hpp"
#include "data/forms/LocForms.hpp" // TextTable
#include "engine/core/Log.hpp"
#include "engine/platform/Paths.hpp" // platform::localTime
#include "engine/ui/UiSystem.hpp"
#include "game/Barter.hpp"
#include "game/SaveGame.hpp" // listSaveSlots
#include "game/ScreenStack.hpp"
#include "game/scenes/GameHud.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/actors/ActorState.hpp"     // gameplay::VendorState
#include "gameplay/actors/CharacterForms.hpp" // gameplay::applyLoadout
#include "gameplay/actors/Followers.hpp"      // canCarry
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/Survival.hpp"
#include "quest/Dialogue.hpp"
#include "world/scene/Components.hpp"

namespace game {

namespace {

// Is the open container a LIVING follower actor? His gear
// obeys the base-kit lock, the carry-weight cap and the auto-equip; a
// corpse (or a plain chest) keeps the old free-transfer behavior —
// owns graves. Resolution = the identity check (ActorForm.
// followerCategory != "", the FollowerController::followerActorForm
// mirror), aliveness = the State.Dead tag the whole scene reads.
const data::ActorForm* livingFollower(const UiRouterContext& ctx,
                                      ecs::Entity entity) {
    if (!entity.is_alive() || !entity.has<world::RefId>() ||
        !entity.has<gameplay::FollowerState>()) {
        return nullptr;
    }
    if (entity.has<gameplay::AbilitySystem>()) {
        if (const auto dead = ctx.gameTags.find("State.Dead");
            dead && entity.get<gameplay::AbilitySystem>().tags.has(*dead)) {
            return nullptr;
        }
    }
    const auto& refId = entity.get<world::RefId>();
    const data::Form* base = ctx.forms.get(refId.base);
    const reflect::TypeInfo* type = ctx.forms.typeOf(refId.base);
    if (!base || !type || !type->isA(data::ActorForm::staticTypeInfo().id)) {
        return nullptr;
    }
    const auto* actor = static_cast<const data::ActorForm*>(base);
    return actor->followerCategory.empty() ? nullptr : actor;
}

// An item's display name for the toasts, whatever its form type
// (reflection — the InteractionController prompt-label pattern).
str itemDisplayName(const UiRouterContext& ctx, const core::Guid& id) {
    const data::FormHandle handle = ctx.forms.handleOf(id);
    if (const data::Form* base = ctx.forms.get(handle)) {
        if (const reflect::TypeInfo* type = ctx.forms.typeOf(handle)) {
            if (const reflect::FieldInfo* field =
                    type->findField("displayName");
                field && field->kind == reflect::FieldKind::Str) {
                const str name = std::get<str>(field->get(base));
                if (!name.empty()) {
                    return name;
                }
            }
        }
        return base->editorId;
    }
    return "?";
}

void toast(const UiRouterContext& ctx, str line) {
    if (ctx.say && !line.empty()) {
        ctx.say(std::move(line));
    }
}

// After items LEFT a follower, his equipment re-aims — a slot whose
// item is gone empties, then the best remaining piece per slot re-equips
// (the auto-equip comparison, reused; his unremovable base kit is the
// usual floor it lands back on).
void reequipFromInventory(const UiRouterContext& ctx,
                          const gameplay::Inventory& items,
                          gameplay::Equipment& equipment) {
    const auto fix = [&](core::Guid& slot) {
        if (slot.isValid() && gameplay::itemCount(items, slot) <= 0) {
            slot = core::Guid {};
        }
    };
    fix(equipment.weapon);
    fix(equipment.head);
    fix(equipment.torso);
    fix(equipment.arms);
    fix(equipment.legs);
    for (const gameplay::ItemStack& stack : items.items) {
        if (stack.count <= 0) {
            continue;
        }
        if (const auto slot =
                gameplay::isUpgrade(ctx.forms, stack.item, equipment)) {
            gameplay::gearSlotRef(equipment, *slot) = stack.item;
        }
    }
}

} // namespace

// --- Inventory / container --------------------------------------------

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
    str title = ctx.texts.get("ui.barter.merchant"); // loc fallback
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
        static_cast<f64>(ctx.statsTuning.vendorRestockHours);
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
    // A LIVING follower's gear has house rules — his
    // base kit never leaves him, his carry weight (× the age factor)
    // rejects the excess, and a strictly better piece auto-equips.
    const data::ActorForm* follower = livingFollower(ctx, containerEntity_);
    if (follower && fromContainer &&
        gameplay::itemUnremovable(ctx.forms, id)) {
        toast(ctx, ctx.texts.format("follower.unremovable",
                                    follower->displayName));
        return;
    }
    if (follower && !fromContainer &&
        containerEntity_.has<gameplay::AbilitySystem>()) {
        const f32 carried = gameplay::inventoryWeight(ctx.forms, loot);
        const f32 unit = gameplay::itemWeight(ctx.forms, id);
        const f32 maxEncumbrance = gameplay::currentValueOf(
            containerEntity_.get<gameplay::AbilitySystem>(),
            gameplay::attr("maxEncumbrance"));
        const f32 ageFactor = gameplay::followerCarryFactor(
            follower->age, ctx.statsTuning);
        if (!gameplay::canCarry(carried, unit, maxEncumbrance, ageFactor)) {
            toast(ctx, ctx.texts.format("follower.gearTooHeavy",
                                        follower->displayName));
            return;
        }
    }
    if (gameplay::removeItem(source, id, 1)) {
        gameplay::addItem(target, id, 1);
        // Auto-equip (docs/FOLLOWERS.md §5): the follower puts on the
        // upgrade the player hands him — strictly better per the SAME
        // power datum the UI's Power column shows (gearPower). The base
        // kit is a floor, not a lock: better replaces it in-slot, the
        // base item just stays in his inventory (unremovable).
        if (follower && containerEntity_.has<gameplay::Equipment>()) {
            auto& equipment =
                containerEntity_.get_mut<gameplay::Equipment>();
            if (!fromContainer) {
                if (const auto slot =
                        gameplay::isUpgrade(ctx.forms, id, equipment)) {
                    gameplay::gearSlotRef(equipment, *slot) = id;
                    toast(ctx, ctx.texts.format(
                                  "follower.autoEquip",
                                  { follower->displayName,
                                    itemDisplayName(ctx, id) }));
                }
            } else {
                // Taking back what he wore: never leave a dangling slot.
                reequipFromInventory(ctx, loot, equipment);
            }
        }
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
            // A LIVING follower keeps his base kit — unremovable
            // stacks stay put; everything else moves as before.
            const data::ActorForm* follower =
                livingFollower(ctx, containerEntity_);
            vector<gameplay::ItemStack> kept;
            for (const gameplay::ItemStack& stack : loot.items) {
                if (stack.count <= 0) {
                    continue;
                }
                if (follower &&
                    gameplay::itemUnremovable(ctx.forms, stack.item)) {
                    kept.push_back(stack);
                    continue;
                }
                gameplay::addItem(bag, stack.item, stack.count);
            }
            loot.items = std::move(kept);
            if (follower && containerEntity_.has<gameplay::Equipment>()) {
                reequipFromInventory(
                    ctx, loot,
                    containerEntity_.get_mut<gameplay::Equipment>());
            }
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

// --- Menus --------------------------------------------------------------

void UiRouter::handleMenuAction(const UiRouterContext& ctx,
                                const str& action) {
    if (action == "resume" || action == "cancel") {
        ctx.screenStack.closeTop();
    } else if (action == "save") {
        // Timestamped manual slot; F5 owns "quick".
        char slot[32];
        const std::time_t now = std::time(nullptr);
        // Platform::localTime (portable — localtime_s is MSVC-only).
        const std::tm local = platform::localTime(now);
        std::strftime(slot, sizeof(slot), "save_%Y%m%d_%H%M%S", &local);
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
        // The scene fills the "options" model (settings + bindings
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
