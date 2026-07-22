#pragma once

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/reflect/Reflect.hpp"

namespace ecs {
class World;
}

namespace gameplay {

// A stack of a given item (a WeaponForm/ItemForm guid). Items are Forms (data);
// the inventory just references them by guid + a count.
struct ItemStack {
    core::Guid item;
    i32 count { 0 };
};

// Runtime component: an actor's carried items. Not reflected (a container);
// its stacks are persisted via the save layer (SavedItem records, SaveState).
struct Inventory {
    vector<ItemStack> items;
};

// The equipped items, by slot (each a Form guid; invalid = empty). Reflected, so
// it serializes and patches like any field. Armor slots contribute their stats
// through gameplay/stats/EquipmentStats; the weapon drives the damage pipeline.
struct Equipment {
    core::Guid weapon;
    core::Guid head;
    core::Guid torso;
    core::Guid arms;
    core::Guid legs;

    REFLECT_BEGIN(Equipment, void)
        REFLECT_FIELD(weapon)
        REFLECT_FIELD(head)
        REFLECT_FIELD(torso)
        REFLECT_FIELD(arms)
        REFLECT_FIELD(legs)
    REFLECT_END()
};

void registerInventoryComponents(ecs::World& world);

// Adds `count` of an item, stacking onto an existing stack if present.
void addItem(Inventory& inventory, const core::Guid& item, i32 count = 1);
// Removes `count`; returns false (and removes nothing) if the stack is short.
bool removeItem(Inventory& inventory, const core::Guid& item, i32 count = 1);
i32 itemCount(const Inventory& inventory, const core::Guid& item);
// Moves EVERY stack of `from` into `to` (stacking onto existing stacks) and
// leaves `from` empty — the burial transfer: a corpse's whole
// inventory lands in the grave. Pure and headless.
void transferAllItems(Inventory& from, Inventory& to);

void equip(Equipment& equipment, const core::Guid& weapon);
void unequip(Equipment& equipment);

} // namespace gameplay
