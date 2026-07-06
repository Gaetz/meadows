#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "game/InventoryView.hpp"
#include "gameplay/inventory/Inventory.hpp"

// Chantier 4 B3: the SkyUI-style table logic behind the inventory,
// container and barter screens.

namespace {

constexpr const char* kItemsToml = R"([plugin]
id = "10000000-0000-4000-8000-000000000001"
name = "items"

[[records]]
form = "10000000-0000-4000-8000-000000000010"
type = "WeaponForm"
new = true
[records.fields]
editorId = "RustySword"
displayName = "Rusty sword"
weight = 3.0
goldValue = 25
slashAttack = 12.0

[[records]]
form = "10000000-0000-4000-8000-000000000011"
type = "ArmorForm"
new = true
[records.fields]
editorId = "LeatherJerkin"
displayName = "Leather jerkin"
slot = "torso"
weight = 4.0
goldValue = 40
armorSlash = 8.0

[[records]]
form = "10000000-0000-4000-8000-000000000012"
type = "ConsumableForm"
new = true
[records.fields]
editorId = "TravelRations"
displayName = "Travel rations"
weight = 0.5
goldValue = 5
restoreHunger = 30.0

[[records]]
form = "10000000-0000-4000-8000-000000000013"
type = "MiscItemForm"
new = true
[records.fields]
editorId = "GoldCoin"
displayName = "Gold"
weight = 0.0
goldValue = 1
)";

core::Guid guid(const char* text) {
    return *core::Guid::fromString(text);
}

struct Fixture {
    data::FormTypeRegistry types;
    data::FormDatabase db;
    gameplay::Inventory inventory;
    gameplay::Equipment equipment;

    core::Guid sword { guid("10000000-0000-4000-8000-000000000010") };
    core::Guid jerkin { guid("10000000-0000-4000-8000-000000000011") };
    core::Guid rations { guid("10000000-0000-4000-8000-000000000012") };
    core::Guid gold { guid("10000000-0000-4000-8000-000000000013") };

    Fixture() {
        data::registerCoreFormTypes(types);
        const auto plugin =
            data::parsePluginToml(kItemsToml, types, "items");
        REQUIRE(plugin.has_value());
        data::resolve({ &*plugin }, types, db);
        gameplay::addItem(inventory, sword, 1);
        gameplay::addItem(inventory, jerkin, 1);
        gameplay::addItem(inventory, rations, 3);
        gameplay::addItem(inventory, gold, 12);
        equipment.weapon = sword;
    }
};

} // namespace

TEST_CASE("InventoryView: rows resolve kind, count, equipped and weight") {
    Fixture f;
    game::InventoryView view;
    view.build(f.db, f.inventory, &f.equipment);

    REQUIRE(view.rows().size() == 4);
    CHECK(view.totalWeight() ==
          doctest::Approx(3.0f + 4.0f + 3 * 0.5f + 0.0f));
    CHECK(view.unresolvedCount() == 0);

    const auto find = [&](const core::Guid& id) {
        for (const auto& row : view.rows()) {
            if (row.id == id) {
                return &row;
            }
        }
        return static_cast<const game::InventoryView::Row*>(nullptr);
    };
    const auto* sword = find(f.sword);
    REQUIRE(sword != nullptr);
    CHECK(sword->equipped);
    CHECK(sword->kind == game::InventoryView::Category::Weapons);
    CHECK(sword->power == doctest::Approx(12.0f));
    const auto* rations = find(f.rations);
    REQUIRE(rations != nullptr);
    CHECK(rations->count == 3);
    CHECK(rations->usable);
}

TEST_CASE("InventoryView: category tabs and search filter") {
    Fixture f;
    game::InventoryView view;
    view.build(f.db, f.inventory, &f.equipment);

    view.setCategory(game::InventoryView::Category::Weapons);
    REQUIRE(view.rows().size() == 1);
    CHECK(view.rows()[0].id == f.sword);

    view.setCategory(game::InventoryView::Category::All);
    view.setSearch("RATion"); // case-insensitive substring
    REQUIRE(view.rows().size() == 1);
    CHECK(view.rows()[0].id == f.rations);

    view.setSearch("");
    CHECK(view.rows().size() == 4);
}

TEST_CASE("InventoryView: column sort with direction toggle") {
    Fixture f;
    game::InventoryView view;
    view.build(f.db, f.inventory, &f.equipment);

    view.sortBy(game::InventoryView::Column::Value); // numbers: descending
    CHECK(view.rows().front().id == f.jerkin);       // 40 first
    CHECK(view.rows().back().id == f.gold);          // 1 last

    view.sortBy(game::InventoryView::Column::Value); // toggle -> ascending
    CHECK(view.rows().front().id == f.gold);

    view.sortBy(game::InventoryView::Column::Name); // names: ascending
    CHECK(view.rows().front().name == "Gold");
}

TEST_CASE("InventoryView: selection survives rebuild, unknown guids skip") {
    Fixture f;
    game::InventoryView view;
    view.build(f.db, f.inventory, &f.equipment);
    view.select(f.rations);

    // A stack whose form no mod provides anymore: skipped, counted.
    gameplay::addItem(f.inventory,
                      guid("deadbeef-0000-4000-8000-000000000000"), 1);
    view.build(f.db, f.inventory, &f.equipment);
    CHECK(view.unresolvedCount() == 1);
    REQUIRE(view.selectedRow() != nullptr);
    CHECK(view.selectedRow()->id == f.rations);
}
