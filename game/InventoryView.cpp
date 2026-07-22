#include "game/InventoryView.hpp"

#include <algorithm>
#include <cctype>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormDatabase.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/EquipmentStats.hpp" // gearPower (one source)

namespace game {

namespace {

str lowered(const str& text) {
    str out = text;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

void InventoryView::build(const data::FormDatabase& forms,
                          const gameplay::Inventory& inventory,
                          const gameplay::Equipment* equipment) {
    all.clear();
    total = 0.0f;
    unresolved = 0;
    for (const gameplay::ItemStack& stack : inventory.items) {
        if (stack.count <= 0) {
            continue;
        }
        const data::FormHandle handle = forms.handleOf(stack.item);
        const data::Form* base = forms.get(handle);
        const reflect::TypeInfo* type = forms.typeOf(handle);
        if (!base || !type) {
            ++unresolved;
            continue;
        }
        Row row;
        row.id = stack.item;
        row.count = stack.count;
        if (type->isA(data::WeaponForm::staticTypeInfo().id)) {
            const auto* weapon = static_cast<const data::WeaponForm*>(base);
            row.name = weapon->displayName;
            row.weight = weapon->weight;
            row.value = weapon->goldValue;
            // Headline power: gameplay::gearPower — the SAME datum the
            // follower auto-equip comparison uses (strongest typed channel, legacy
            // `damage` as fallback for 2D-era records).
            row.power = gameplay::gearPower(forms, stack.item);
            row.kind = Category::Weapons;
        } else if (type->isA(data::ArmorForm::staticTypeInfo().id)) {
            const auto* armor = static_cast<const data::ArmorForm*>(base);
            row.name = armor->displayName;
            row.weight = armor->weight;
            row.value = armor->goldValue;
            row.power = gameplay::gearPower(forms, stack.item);
            row.kind = Category::Armor;
        } else if (type->isA(data::ConsumableForm::staticTypeInfo().id)) {
            const auto* item = static_cast<const data::ConsumableForm*>(base);
            row.name = item->displayName;
            row.weight = item->weight;
            row.value = item->goldValue;
            row.kind = Category::Consumables;
            row.usable = true;
        } else if (type->isA(data::MiscItemForm::staticTypeInfo().id)) {
            const auto* item = static_cast<const data::MiscItemForm*>(base);
            row.name = item->displayName;
            row.weight = item->weight;
            row.value = item->goldValue;
            row.kind = Category::Misc;
        } else {
            ++unresolved; // not an item kind this view knows
            continue;
        }
        if (row.name.empty()) {
            row.name = base->editorId;
        }
        if (equipment) {
            row.equipped = equipment->weapon == row.id ||
                           equipment->head == row.id ||
                           equipment->torso == row.id ||
                           equipment->arms == row.id ||
                           equipment->legs == row.id;
        }
        total += row.weight * static_cast<f32>(row.count);
        all.push_back(std::move(row));
    }
    refresh();
}

void InventoryView::setCategory(Category category) {
    activeCategory = category;
    refresh();
}

void InventoryView::setSearch(str needle) {
    searchNeedle = std::move(needle);
    refresh();
}

void InventoryView::sortBy(Column column) {
    if (activeColumn == column) {
        ascending = !ascending;
    } else {
        activeColumn = column;
        ascending = column == Column::Name; // numbers default descending
    }
    refresh();
}

const InventoryView::Row* InventoryView::selectedRow() const {
    const auto it =
        std::find_if(visible.begin(), visible.end(),
                     [&](const Row& row) { return row.id == selection; });
    return it != visible.end() ? &*it : nullptr;
}

void InventoryView::refresh() {
    visible.clear();
    const str needle = lowered(searchNeedle);
    for (const Row& row : all) {
        if (activeCategory != Category::All && row.kind != activeCategory) {
            continue;
        }
        if (!needle.empty() &&
            lowered(row.name).find(needle) == str::npos) {
            continue;
        }
        visible.push_back(row);
    }
    const bool asc = ascending;
    const Column column = activeColumn;
    std::stable_sort(visible.begin(), visible.end(),
                     [asc, column](const Row& a, const Row& b) {
                         const auto ordered = [asc](auto x, auto y) {
                             return asc ? x < y : y < x;
                         };
                         switch (column) {
                         case Column::Weight:
                             return ordered(a.weight, b.weight);
                         case Column::Value:
                             return ordered(a.value, b.value);
                         case Column::Power:
                             return ordered(a.power, b.power);
                         case Column::Name:
                         default:
                             return ordered(lowered(a.name), lowered(b.name));
                         }
                     });
}

} // namespace game
