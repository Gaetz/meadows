#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/core/Rng.hpp"
#include "game/Barter.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "gameplay/inventory/Inventory.hpp"

// Barter transactions + LoadoutEntryForm rolls.

namespace {

core::Guid guid(const char* text) {
    return *core::Guid::fromString(text);
}

const core::Guid kGold = guid("20000000-0000-4000-8000-000000000001");
const core::Guid kRations = guid("20000000-0000-4000-8000-000000000002");

} // namespace

TEST_CASE("barter: prices floor at one coin") {
    CHECK(game::barterPrice(10, 1.5f) == 15);
    CHECK(game::barterPrice(10, 0.5f) == 5);
    CHECK(game::barterPrice(1, 0.5f) == 1);  // never free
    CHECK(game::barterPrice(0, 1.5f) == 1);
}

TEST_CASE("barter: buy and sell move item + coins, fail untouched") {
    gameplay::Inventory player;
    gameplay::Inventory vendor;
    gameplay::addItem(player, kGold, 20);
    gameplay::addItem(vendor, kRations, 2);
    gameplay::addItem(vendor, kGold, 3);

    // Buy at 8: coins go vendor-ward, the item player-ward.
    REQUIRE(game::barterBuy(player, vendor, kRations, 8, kGold));
    CHECK(gameplay::itemCount(player, kRations) == 1);
    CHECK(gameplay::itemCount(player, kGold) == 12);
    CHECK(gameplay::itemCount(vendor, kGold) == 11);

    // Too expensive: nothing moves.
    CHECK_FALSE(game::barterBuy(player, vendor, kRations, 999, kGold));
    CHECK(gameplay::itemCount(player, kRations) == 1);
    CHECK(gameplay::itemCount(player, kGold) == 12);

    // Sell at 3: the vendor pays.
    REQUIRE(game::barterSell(player, vendor, kRations, 3, kGold));
    CHECK(gameplay::itemCount(player, kRations) == 0);
    CHECK(gameplay::itemCount(player, kGold) == 15);
    CHECK(gameplay::itemCount(vendor, kRations) == 2);

    // The vendor's wealth is limited: an unaffordable sale fails.
    CHECK_FALSE(game::barterSell(player, vendor, kGold, 1, kGold)); // gold-for-gold blocked
    gameplay::addItem(player, kRations, 1);
    CHECK_FALSE(game::barterSell(player, vendor, kRations, 999, kGold));
    CHECK(gameplay::itemCount(player, kRations) == 1);
}

TEST_CASE("loadout: child records roll into the inventory") {
    constexpr const char* kToml = R"([plugin]
id = "20000000-0000-4000-8000-000000000100"
name = "loadout"

[[records]]
form = "20000000-0000-4000-8000-000000000010"
type = "ActorForm"
new = true
[records.fields]
editorId = "Merchant"

[[records]]
form = "20000000-0000-4000-8000-000000000011"
type = "LoadoutEntryForm"
new = true
[records.fields]
parent = "20000000-0000-4000-8000-000000000010"
item = "20000000-0000-4000-8000-000000000002"
count = 4

[[records]]
form = "20000000-0000-4000-8000-000000000012"
type = "LoadoutEntryForm"
new = true
[records.fields]
parent = "20000000-0000-4000-8000-000000000010"
item = "20000000-0000-4000-8000-000000000001"
count = 60

[[records]]
form = "20000000-0000-4000-8000-000000000013"
type = "LoadoutEntryForm"
new = true
[records.fields]
parent = "20000000-0000-4000-8000-000000000010"
item = "20000000-0000-4000-8000-000000000002"
count = 1
chance = 0.0
)";
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    gameplay::registerCharacterFormTypes(types);
    const auto plugin = data::parsePluginToml(kToml, types, "loadout");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    gameplay::Inventory bag;
    core::Rng rng { 42 };
    gameplay::applyLoadout(db, guid("20000000-0000-4000-8000-000000000010"),
                           bag, rng);
    CHECK(gameplay::itemCount(bag, kRations) == 4); // chance-0 entry skipped
    CHECK(gameplay::itemCount(bag, kGold) == 60);
}
