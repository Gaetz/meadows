#include "gameplay/inventory/Inventory.hpp"

#include "engine/ecs/World.hpp"

namespace gameplay {

void registerInventoryComponents(ecs::World& world) {
    world.handle().component<Inventory>(); // runtime-only (container)
    world.registerComponent<Equipment>();  // reflected
}

void addItem(Inventory& inventory, const core::Guid& item, i32 count) {
    for (ItemStack& stack : inventory.items) {
        if (stack.item == item) {
            stack.count += count;
            return;
        }
    }
    inventory.items.push_back({ item, count });
}

bool removeItem(Inventory& inventory, const core::Guid& item, i32 count) {
    for (auto it = inventory.items.begin(); it != inventory.items.end(); ++it) {
        if (it->item == item) {
            if (it->count < count) {
                return false;
            }
            it->count -= count;
            if (it->count == 0) {
                inventory.items.erase(it);
            }
            return true;
        }
    }
    return false;
}

i32 itemCount(const Inventory& inventory, const core::Guid& item) {
    for (const ItemStack& stack : inventory.items) {
        if (stack.item == item) {
            return stack.count;
        }
    }
    return 0;
}

void equip(Equipment& equipment, const core::Guid& weapon) {
    equipment.weapon = weapon;
}

void unequip(Equipment& equipment) {
    equipment.weapon = {};
}

} // namespace gameplay
