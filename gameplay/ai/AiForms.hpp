#pragma once

#include "data/forms/Form.hpp"

// AI data Forms (horizontal pass H1) — the Skyrim-NPC layer cake:
//   ScheduleForm (WHEN/WHERE/WHAT, per day slice, the daily routine)
//     -> AiPackageForm (an executable behavior: sleep, work, wander...)
//        -> movement/furniture/abilities (the verticals execute these)
// Schedule ENTRIES are child records (ScheduleEntryForm.parent) — a mod
// inserts "goes to the tavern at 19h" into any NPC's day by adding ONE
// record. Entry conditions reuse ConditionForm (parent = the entry guid).
//
// HOW TO FILL (post-7/07): ScheduleSystem (H7 skeleton) picks the active
// entry; the package vertical executes kind-specific behavior (navmesh
// travel, furniture use via FurnitureForm, wander radius around
// `location`). Combat/dialogue interrupt and resume (H7 exposes intent).

namespace data {
class FormTypeRegistry;
}

namespace gameplay {

// One executable behavior. `kind` routes to a C++ executor (fixed set,
// §2.7); `location` is a marker/furniture/cell reference guid.
struct AiPackageForm : data::Form {
    str kind { "wander" }; // sleep | eat | work | wander | travel |
                           // useFurniture | guard
    core::Guid location;   // marker or furniture REFERENCE guid
    f32 radius { 4.0f };   // wander/guard reach around the location
    f32 speed { 1.0f };    // walk-speed multiplier (stroll vs hurry)

    REFLECT_BEGIN(AiPackageForm, data::Form)
        REFLECT_FIELD(kind)
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
