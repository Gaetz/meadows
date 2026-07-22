#pragma once

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

namespace data {
class FormDatabase;
}
namespace gameplay {
struct Inventory;
struct Equipment;
}

namespace game {

// The SkyUI-style inventory table logic — PURE and
// headless-testable: category tabs, name search, sortable columns,
// persistent selection/sort state. The .rml only displays the rows this
// class produces; the barter and container screens reuse it (one view per
// panel). Prices are the barter screen's concern — this view exposes
// the raw goldValue.
class InventoryView {
public:
    enum class Category : u8 { All, Weapons, Armor, Consumables, Misc };
    enum class Column : u8 { Name, Weight, Value, Power };

    struct Row {
        core::Guid id;
        str name;
        i32 count { 0 };
        f32 weight { 0.0f };  // per unit
        i32 value { 0 };      // per unit (goldValue)
        f32 power { 0.0f };   // weapon damage / armor slash mitigation
        Category kind { Category::Misc };
        bool equipped { false };
        bool usable { false }; // consumable
    };

    // Rebuilds the row set from an inventory (+ optional equipment for the
    // "equipped" flag). Unresolvable item guids are skipped with a count
    // in `unresolved` (a mod removed the form — never fatal, §5).
    void build(const data::FormDatabase& forms,
               const gameplay::Inventory& inventory,
               const gameplay::Equipment* equipment);

    void setCategory(Category category);
    Category category() const { return activeCategory; }
    void setSearch(str needle); // case-insensitive substring on the name
    const str& search() const { return searchNeedle; }
    // Clicking the active column flips the direction (SkyUI behavior).
    void sortBy(Column column);
    Column sortColumn() const { return activeColumn; }
    bool sortAscending() const { return ascending; }

    void select(const core::Guid& id) { selection = id; }
    const core::Guid& selected() const { return selection; }
    // The selected row, if it survived filters/rebuild.
    const Row* selectedRow() const;

    // Filtered + sorted view of the last build().
    const vector<Row>& rows() const { return visible; }
    f32 totalWeight() const { return total; } // ALL items, unfiltered
    u32 unresolvedCount() const { return unresolved; }

private:
    void refresh(); // filters + sorts `all` into `visible`

    vector<Row> all;
    vector<Row> visible;
    Category activeCategory { Category::All };
    str searchNeedle;
    Column activeColumn { Column::Name };
    bool ascending { true };
    core::Guid selection {};
    f32 total { 0.0f };
    u32 unresolved { 0 };
};

} // namespace game
