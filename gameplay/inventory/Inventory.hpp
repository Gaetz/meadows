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
// serialization is Phase 8 (same deferred container story as active effects).
struct Inventory {
    vector<ItemStack> items;
};

// The currently equipped weapon (a WeaponForm guid; invalid = nothing equipped).
// Reflected, so it serializes and patches like any field.
struct Equipment {
    core::Guid weapon;

    REFLECT_BEGIN(Equipment, void)
        REFLECT_FIELD(weapon)
    REFLECT_END()
};

void registerInventoryComponents(ecs::World& world);

// Adds `count` of an item, stacking onto an existing stack if present.
void addItem(Inventory& inventory, const core::Guid& item, i32 count = 1);
// Removes `count`; returns false (and removes nothing) if the stack is short.
bool removeItem(Inventory& inventory, const core::Guid& item, i32 count = 1);
i32 itemCount(const Inventory& inventory, const core::Guid& item);

void equip(Equipment& equipment, const core::Guid& weapon);
void unequip(Equipment& equipment);

} // namespace gameplay
