#pragma once

#include <iterator>
#include <optional>
#include <string_view>

#include "data/forms/Form.hpp"

// AI data Forms — the Skyrim-NPC layer cake:
//   ScheduleForm (WHEN/WHERE/WHAT, per day slice, the daily routine)
//     -> AiPackageForm (an executable behavior: sleep, work, wander...)
//        -> movement/furniture/abilities (the verticals execute these)
// Schedule ENTRIES are child records (ScheduleEntryForm.parent) — a mod
// inserts "goes to the tavern at 19h" into any NPC's day by adding ONE
// record. Entry conditions reuse ConditionForm (parent = the entry guid).
//
// HOW TO FILL: ScheduleSystem picks the active
// entry; the package vertical executes kind-specific behavior (navmesh
// travel, furniture use via FurnitureForm, wander radius around
// `location`). Combat/dialogue interrupt and resume (ScheduleSystem
// exposes the intent).

namespace data {
class FormTypeRegistry;
}

namespace gameplay {

// The package kinds (AiPackageForm.kind): each routes to a C++ executor
// (fixed set, §2.7).
inline constexpr std::string_view kAiPackageKindNames[] = {
    "sleep", "eat", "work", "wander", "travel", "useFurniture", "guard" };
inline constexpr reflect::EnumTable kAiPackageKinds { kAiPackageKindNames };

// The typed view of kAiPackageKinds: one enumerator per table entry, in
// table order (pinned below). Executors parse the field ONCE through
// parsePackageKind and switch on the result — never compare the string.
enum class PackageKind : u8 {
    Sleep,
    Eat,
    Work,
    Wander,
    Travel,
    UseFurniture,
    Guard,
    Count
};
static_assert(std::size(kAiPackageKindNames) ==
                  static_cast<size_t>(PackageKind::Count),
              "kAiPackageKindNames and PackageKind must list the same kinds");

// nullopt = a value outside the table (cooked data that bypassed the
// loader's check); the caller decides the fallback and logs.
inline std::optional<PackageKind> parsePackageKind(std::string_view kind) {
    return reflect::parseEnum<PackageKind>(kAiPackageKinds, kind);
}

// One executable behavior. `kind` routes to a C++ executor (fixed set,
// §2.7); `location` is a marker/furniture/cell reference guid.
struct AiPackageForm : data::Form {
    str kind { "wander" }; // kAiPackageKinds
    core::Guid location;   // marker or furniture REFERENCE guid
    f32 radius { 4.0f };   // wander/guard reach around the location
    f32 speed { 1.0f };    // walk-speed multiplier (stroll vs hurry)

    REFLECT_BEGIN(AiPackageForm, data::Form)
        REFLECT_FIELD_ENUM(kind, kAiPackageKinds)
        REFLECT_FIELD(location)
        REFLECT_FIELD(radius)
        REFLECT_FIELD(speed)
    REFLECT_END()
};

// A daily routine. Owns nothing directly: its entries point back at it.
struct ScheduleForm : data::Form {
    REFLECT_BEGIN(ScheduleForm, data::Form)
    REFLECT_END()
};

// One slice of the day. Hours are game-clock hours [0,24); an entry
// wrapping midnight uses startHour > endHour (e.g. 22 -> 6). When several
// entries overlap, the LAST one in load order wins (deterministic, and a
// mod can therefore override a slice by adding a later entry).
struct ScheduleEntryForm : data::Form {
    core::Guid parent;  // ScheduleForm
    f32 startHour { 8.0f };
    f32 endHour { 18.0f };
    core::Guid package; // AiPackageForm
    core::Guid location; // overrides the package's location if set

    REFLECT_BEGIN(ScheduleEntryForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(startHour)
        REFLECT_FIELD(endHour)
        REFLECT_FIELD(package)
        REFLECT_FIELD(location)
    REFLECT_END()
};

void registerAiFormTypes(data::FormTypeRegistry& registry);

} // namespace gameplay
