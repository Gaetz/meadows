#include "game/scenes/UiRouter.hpp"

#include <charconv>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>

#include <glm/glm.hpp>

#include "data/forms/CoreForms.hpp" // ActorForm, WeaponForm, ArmorForm...
#include "data/forms/FormDatabase.hpp"
#include "data/forms/LocForms.hpp"     // TextTable
#include "data/forms/Vocabularies.hpp" // kArmorSlots
#include "engine/core/Log.hpp"
#include "engine/reflect/Reflect.hpp"
#include "engine/platform/Paths.hpp" // platform::localTime
#include "engine/ui/UiSystem.hpp"
#include "game/Barter.hpp"
#include "game/SaveGame.hpp" // listSaveSlots
#include "game/ScreenStack.hpp"
#include "game/scenes/GameHud.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/stats/StatusBuildup.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/actors/ActorState.hpp"     // gameplay::VendorState
#include "gameplay/actors/CharacterForms.hpp" // gameplay::applyLoadout
#include "gameplay/actors/FollowerProgression.hpp" // canCarry
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/StatsTuning.hpp"
#include "gameplay/stats/Survival.hpp"
#include "quest/Dialogue.hpp"
#include "world/scene/Components.hpp"

namespace game {

namespace {

// --- The data-event vocabulary ------------------------------------------

// The models whose events reach the router (the scene hands options/map/
// recruit to their own controllers first) and the events those models
// declare (LandscapeScene createModel). Both parse through
// reflect::parseEnum; the route table in handleUiEvent says which event
// lands on which model.
enum class UiModel : u8 {
    Inventory,
    Container,
    Barter,
    Menu,
    Saves,
    Journal,
    Dialogue,
    Count
};
constexpr std::string_view kUiModelNames[] = { "inventory", "container",
                                               "barter",    "menu",
                                               "saves",     "journal",
                                               "dialogue" };
constexpr reflect::EnumTable kUiModels { kUiModelNames };
static_assert(std::size(kUiModelNames) == static_cast<size_t>(UiModel::Count));

enum class UiEvent : u8 {
    Tab,
    SortCol,
    Pick,
    EquipAction,
    UseAction,
    PickLoot,
    TakeAll,
    PickBuy,
    MenuAction,
    LoadSlot,
    LoadCancel,
    JournalClose,
    Choose,
    Count
};
constexpr std::string_view kUiEventNames[] = {
    "tab",      "sortCol",    "pick",     "equipAction", "useAction",
    "pickLoot", "takeAll",    "pickBuy",  "menuAction",  "loadSlot",
    "loadCancel", "journalClose", "choose" };
constexpr reflect::EnumTable kUiEvents { kUiEventNames };
static_assert(std::size(kUiEventNames) == static_cast<size_t>(UiEvent::Count));

using UiEventHandler = void (UiRouter::*)(const UiRouterContext&,
                                          const vector<str>&);
struct UiRoute {
    UiModel model;
    UiEvent event;
    UiEventHandler handler;
};

// The item screens re-push their models after EVERY event they receive:
// rows, gold line and detail pane all read router state.
constexpr bool refreshesItemModels(UiModel model) {
    switch (model) {
    case UiModel::Inventory:
    case UiModel::Container:
    case UiModel::Barter:
        return true;
    case UiModel::Menu:
    case UiModel::Saves:
    case UiModel::Journal:
    case UiModel::Dialogue:
    case UiModel::Count:
        return false;
    }
    return false;
}

// The shared menu vocabulary (pause/main/wait/workshop screens). WaitHours
// has no name of its own: "wait<N>" carries its duration in the action
// string and is parsed structurally — it closes the named table.
enum class MenuAction : u8 {
    Resume,
    Cancel,
    Save,
    LoadMenu,
    Wait,
    Play,
    PlayStory,
    PlaySandbox,
    MainMenu,
    Options,
    Quit,
    WaitHours
};
constexpr std::string_view kMenuActionNames[] = {
    "resume",   "cancel",       "save",     "loadmenu", "wait",   "play",
    "play-story", "play-sandbox", "mainmenu", "options",  "quit" };
constexpr reflect::EnumTable kMenuActions { kMenuActionNames };
static_assert(std::size(kMenuActionNames) ==
              static_cast<size_t>(MenuAction::WaitHours));
constexpr std::string_view kWaitPrefix = "wait";

struct MenuCommand {
    MenuAction action;
    f32 hours { 0.0f }; // WaitHours only
};

std::optional<MenuCommand> parseMenuCommand(std::string_view text) {
    if (const auto action =
            reflect::parseEnum<MenuAction>(kMenuActions, text)) {
        return MenuCommand { .action = *action };
    }
    if (text.starts_with(kWaitPrefix) && text.size() > kWaitPrefix.size()) {
        const std::string_view digits = text.substr(kWaitPrefix.size());
        u32 hours = 0;
        const auto [end, error] = std::from_chars(
            digits.data(), digits.data() + digits.size(), hours);
        if (error == std::errc {} && end == digits.data() + digits.size()) {
            return MenuCommand { .action = MenuAction::WaitHours,
                                 .hours = static_cast<f32>(hours) };
        }
    }
    return std::nullopt;
}

// The dialogue row id that ends the conversation (the option tagged
// 'leave' in dialogue.rml).
constexpr std::string_view kLeaveOption = "leave";

// GearSlot's armor members follow the kArmorSlots vocabulary order right
// after Weapon, so an ArmorForm.slot ordinal maps by offset.
constexpr gameplay::GearSlot armorGearSlot(u32 armorSlotOrdinal) {
    return static_cast<gameplay::GearSlot>(armorSlotOrdinal + 1);
}
static_assert(armorGearSlot(0) == gameplay::GearSlot::Head &&
              armorGearSlot(static_cast<u32>(
                  data::vocab::kArmorSlots.size() - 1)) ==
                  gameplay::GearSlot::Legs);

// A name no table accepts is a document/model mismatch (a modded .rml
// emitting what the scene never declared): one warning per distinct
// line, not one per click.
void warnOnce(str line) {
    static vector<str> reported;
    for (const str& seen : reported) {
        if (seen == line) {
            return;
        }
    }
    LOG_WARN("{}", line);
    reported.push_back(std::move(line));
}

std::optional<core::Guid> argGuid(const vector<str>& args) {
    return args.empty() ? std::nullopt : core::Guid::fromString(args[0]);
}

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
    if (!ctx.playerEntity.has<gameplay::Inventory>() ||
        !containerEntity_.has<gameplay::Inventory>()) {
        LOG_WARN("barter: a side has no Inventory — setup bug, trade skipped");
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
        if (const auto ordinal =
                data::vocab::kArmorSlots.indexOf(armor->slot)) {
            core::Guid& slot =
                gameplay::gearSlotRef(equipment, armorGearSlot(*ordinal));
            slot = slot == id ? core::Guid {} : id;
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
            // WITH the buildup pointer: an effect carrying buildupType
            // (antidote, poison vial) routes through tryAddBuildup —
            // without it applyEffect no-ops that whole family.
            gameplay::applyEffect(
                ctx.playerEntity.get_mut<gameplay::AttributeSet>(),
                ctx.playerEntity.get_mut<gameplay::AbilitySystem>(), *effect,
                ctx.gameTags,
                ctx.playerEntity.has<gameplay::StatusBuildup>()
                    ? &ctx.playerEntity.get_mut<gameplay::StatusBuildup>()
                    : nullptr);
        }
    }
    LOG_INFO("Used: {}", consumable->editorId);
}

// --- The data-event dispatch ----------------------------------------------------------

void UiRouter::handleUiEvent(const UiRouterContext& ctx, const str& model,
                             const str& event, const vector<str>& args) {
    // Lives in a member so the pointers to the private handlers can be
    // formed; one row per (model, event) the documents may emit.
    static constexpr UiRoute kRoutes[] = {
        { UiModel::Inventory, UiEvent::Tab, &UiRouter::onInventoryTab },
        { UiModel::Inventory, UiEvent::SortCol, &UiRouter::onInventorySortCol },
        { UiModel::Inventory, UiEvent::Pick, &UiRouter::onInventoryPick },
        { UiModel::Inventory, UiEvent::EquipAction,
          &UiRouter::onInventoryEquip },
        { UiModel::Inventory, UiEvent::UseAction, &UiRouter::onInventoryUse },
        { UiModel::Container, UiEvent::PickLoot,
          &UiRouter::onContainerPickLoot },
        { UiModel::Container, UiEvent::TakeAll, &UiRouter::onContainerTakeAll },
        { UiModel::Barter, UiEvent::PickBuy, &UiRouter::onBarterPickBuy },
        { UiModel::Menu, UiEvent::MenuAction, &UiRouter::onMenuAction },
        { UiModel::Saves, UiEvent::LoadSlot, &UiRouter::onSavesLoadSlot },
        { UiModel::Saves, UiEvent::LoadCancel, &UiRouter::onSavesLoadCancel },
        { UiModel::Journal, UiEvent::JournalClose, &UiRouter::onJournalClose },
        { UiModel::Dialogue, UiEvent::Choose, &UiRouter::onDialogueChoose },
    };
    const auto modelId = reflect::parseEnum<UiModel>(kUiModels, model);
    const auto eventId = reflect::parseEnum<UiEvent>(kUiEvents, event);
    const UiRoute* route = nullptr;
    if (modelId && eventId) {
        for (const UiRoute& candidate : kRoutes) {
            if (candidate.model == *modelId && candidate.event == *eventId) {
                route = &candidate;
                break;
            }
        }
    }
    if (route) {
        (this->*route->handler)(ctx, args);
    } else {
        warnOnce("ui: no route for event '" + event + "' on model '" + model +
                 "'");
    }
    if (modelId && refreshesItemModels(*modelId)) {
        ctx.pushItemModels();
    }
}

void UiRouter::onInventoryTab(const UiRouterContext& ctx,
                              const vector<str>& args) {
    if (args.empty()) {
        return;
    }
    using Category = InventoryView::Category;
    // An unknown tab name lands on the All tab.
    ctx.hud.inventory().setCategory(
        reflect::parseEnum<Category>(kInventoryCategories, args[0])
            .value_or(Category::All));
}

void UiRouter::onInventorySortCol(const UiRouterContext& ctx,
                                  const vector<str>& args) {
    if (args.empty()) {
        return;
    }
    using Column = InventoryView::Column;
    // An unknown column name sorts by name.
    ctx.hud.inventory().sortBy(
        reflect::parseEnum<Column>(kInventoryColumns, args[0])
            .value_or(Column::Name));
}

void UiRouter::onInventoryPick(const UiRouterContext& ctx,
                               const vector<str>& args) {
    const auto id = argGuid(args);
    if (!id) {
        return;
    }
    if (barterMode_) {
        barterTrade(ctx, *id, /*playerBuys=*/false); // sell
    } else if (containerEntity_.is_alive()) {
        transferItem(ctx, *id, /*fromContainer=*/false);
    } else {
        ctx.hud.inventory().select(*id);
    }
}

void UiRouter::onInventoryEquip(const UiRouterContext& ctx,
                                const vector<str>& /*args*/) {
    toggleEquip(ctx, ctx.hud.inventory().selected());
}

void UiRouter::onInventoryUse(const UiRouterContext& ctx,
                              const vector<str>& /*args*/) {
    useConsumable(ctx, ctx.hud.inventory().selected());
}

void UiRouter::onContainerPickLoot(const UiRouterContext& ctx,
                                   const vector<str>& args) {
    if (const auto id = argGuid(args)) {
        transferItem(ctx, *id, /*fromContainer=*/true);
    }
}

void UiRouter::onContainerTakeAll(const UiRouterContext& ctx,
                                  const vector<str>& /*args*/) {
    if (!containerEntity_.is_alive() || !ctx.playerEntity.is_alive()) {
        return;
    }
    auto& loot = containerEntity_.get_mut<gameplay::Inventory>();
    auto& bag = ctx.playerEntity.get_mut<gameplay::Inventory>();
    // A LIVING follower keeps his base kit — unremovable
    // stacks stay put; everything else moves as before.
    const data::ActorForm* follower = livingFollower(ctx, containerEntity_);
    vector<gameplay::ItemStack> kept;
    for (const gameplay::ItemStack& stack : loot.items) {
        if (stack.count <= 0) {
            continue;
        }
        if (follower && gameplay::itemUnremovable(ctx.forms, stack.item)) {
            kept.push_back(stack);
            continue;
        }
        gameplay::addItem(bag, stack.item, stack.count);
    }
    loot.items = std::move(kept);
    if (follower && containerEntity_.has<gameplay::Equipment>()) {
        reequipFromInventory(ctx, loot,
                             containerEntity_.get_mut<gameplay::Equipment>());
    }
}

void UiRouter::onBarterPickBuy(const UiRouterContext& ctx,
                               const vector<str>& args) {
    if (const auto id = argGuid(args)) {
        barterTrade(ctx, *id, /*playerBuys=*/true);
    }
}

void UiRouter::onMenuAction(const UiRouterContext& ctx,
                            const vector<str>& args) {
    if (!args.empty()) {
        handleMenuAction(ctx, args[0]);
    }
}

void UiRouter::onSavesLoadSlot(const UiRouterContext& ctx,
                               const vector<str>& args) {
    if (args.empty()) {
        return;
    }
    ctx.screenStack.closeAll();
    ctx.requestLoad(args[0]);
}

void UiRouter::onSavesLoadCancel(const UiRouterContext& ctx,
                                 const vector<str>& /*args*/) {
    ctx.screenStack.closeTop();
}

void UiRouter::onJournalClose(const UiRouterContext& ctx,
                              const vector<str>& /*args*/) {
    ctx.screenStack.closeTop();
}

void UiRouter::onDialogueChoose(const UiRouterContext& ctx,
                                const vector<str>& args) {
    if (args.empty() || !ctx.dialogueRunner) {
        return;
    }
    if (args[0] == kLeaveOption) {
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

// --- Menus --------------------------------------------------------------

void UiRouter::handleMenuAction(const UiRouterContext& ctx,
                                const str& action) {
    const auto command = parseMenuCommand(action);
    if (!command) {
        warnOnce("ui: unknown menu action '" + action + "'");
        return;
    }
    switch (command->action) {
    case MenuAction::Resume:
    case MenuAction::Cancel:
        ctx.screenStack.closeTop();
        break;
    case MenuAction::Save: {
        // Timestamped manual slot; F5 owns "quick".
        char slot[32];
        const std::time_t now = std::time(nullptr);
        // Platform::localTime (portable — localtime_s is MSVC-only).
        const std::tm local = platform::localTime(now);
        std::strftime(slot, sizeof(slot), "save_%Y%m%d_%H%M%S", &local);
        ctx.performSave(slot);
        ctx.screenStack.closeTop();
        break;
    }
    case MenuAction::LoadMenu: {
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
        break;
    }
    case MenuAction::Wait:
        ctx.updateMenuClockLine();
        ctx.screenStack.show("wait");
        break;
    case MenuAction::WaitHours:
        ctx.wait(command->hours);
        ctx.updateMenuClockLine();
        ctx.screenStack.closeTop();
        break;
    case MenuAction::Play:
        ctx.screenStack.closeAll();
        if (!ctx.playMode) {
            ctx.enterPlayMode();
        }
        break;
    case MenuAction::PlayStory:
    case MenuAction::PlaySandbox:
        // Main-menu world pick: the scene's closure owns the whole game
        // entry (world flipped BEFORE the capsule spawns, menus closed,
        // Play entered) — one body shared with the boot override.
        if (ctx.beginPlay) {
            ctx.beginPlay(command->action == MenuAction::PlaySandbox);
            break;
        }
        ctx.screenStack.closeAll();
        if (!ctx.playMode) {
            ctx.enterPlayMode();
        }
        break;
    case MenuAction::MainMenu:
        if (ctx.playMode) {
            ctx.exitPlayMode();
        }
        ctx.screenStack.closeAll();
        ctx.updateMenuClockLine();
        ctx.screenStack.show("mainmenu");
        break;
    case MenuAction::Options:
        // The scene fills the "options" model (settings + bindings
        // are its state) and shows the screen over the current menu.
        ctx.openOptions();
        break;
    case MenuAction::Quit:
        ctx.requestQuit();
        break;
    }
}

void UiRouter::reset() {
    containerEntity_ = ecs::Entity {};
    barterMode_ = false;
    vendorBuyMult_ = 1.5f;
    vendorSellMult_ = 0.5f;
}

} // namespace game
