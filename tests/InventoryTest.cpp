#include <doctest/doctest.h>

#include "gameplay/inventory/Inventory.hpp"

using core::Guid;
using namespace gameplay;

namespace {
const Guid kSword = *Guid::fromString("11110000-0000-4000-8000-000000000001");
const Guid kPotion = *Guid::fromString("11110000-0000-4000-8000-000000000002");
const Guid kUnknown = *Guid::fromString("11110000-0000-4000-8000-0000000000ff");
} // namespace

TEST_CASE("inventory: add stacks, remove respects counts") {
    Inventory inventory;
    addItem(inventory, kSword);
    addItem(inventory, kSword, 2);
    addItem(inventory, kPotion, 5);

    CHECK(itemCount(inventory, kSword) == 3);
    CHECK(itemCount(inventory, kPotion) == 5);
    CHECK(itemCount(inventory, kUnknown) == 0); // not present

    CHECK(removeItem(inventory, kSword, 2));
    CHECK(itemCount(inventory, kSword) == 1);

    CHECK_FALSE(removeItem(inventory, kSword, 5)); // not enough, no change
    CHECK(itemCount(inventory, kSword) == 1);

    CHECK(removeItem(inventory, kSword, 1)); // empties the stack
    CHECK(itemCount(inventory, kSword) == 0);
}

TEST_CASE("inventory: equip / unequip a weapon") {
    Equipment equipment;
    CHECK_FALSE(equipment.weapon.isValid());

    equip(equipment, kSword);
    CHECK(equipment.weapon == kSword);

    unequip(equipment);
    CHECK_FALSE(equipment.weapon.isValid());
}
