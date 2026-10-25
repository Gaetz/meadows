#pragma once

#include <iterator>
#include <string_view>

#include "data/forms/Form.hpp"
#include "engine/core/Defines.hpp"

// Furniture & workstations — the SHARED player/NPC
// object-interaction system: beds, chairs, forges, alchemy tables. What
// makes NPC schedules VISIBLE. A furniture piece is a placeable base form
// (Furniture spawn category); its use points are child records.
//
// HOW TO FILL: the Furniture vertical (the occupancy API exists) walks
// the user to the point, plays `animTag` through the
// anim graph, applies `effect` (GAS) while used — sleeping IS the
// rest effect, crafting stations open their UI screen for the player.

namespace data {
class FormTypeRegistry;
}

namespace gameplay {

// What the player's interact key DOES on a piece of furniture. `Use` is
// the generic path (rest/sleep, or the workstation screen); the others
// route to their dedicated handlers. kFurnitureInteractions names them in
// this order; "" = unset (derived from `category`, see
// furnitureInteraction).
enum class FurnitureInteraction : u8 {
    Use,
    Grave,     // homage on interact, the dead follower's inventory on alt
    Mount,     // ride it
    Container, // loot it
    Lever,     // pull it
    Count
};
inline constexpr std::string_view kFurnitureInteractionNames[] = {
    "use", "grave", "mount", "container", "lever" };
inline constexpr reflect::EnumTable kFurnitureInteractions {
    kFurnitureInteractionNames, true };
static_assert(std::size(kFurnitureInteractionNames) ==
              static_cast<size_t>(FurnitureInteraction::Count));

// `interaction` and `restHours` are the AUTHORITATIVE behaviour fields;
// `category` is descriptive once they are set. Records that leave them
// unset fall back on the category through the resolvers below
// (furnitureInteraction / furnitureRestHours), so existing data keeps
// working unchanged — always read the resolvers, never the raw fields.
struct FurnitureForm : data::Form {
    str displayName;
    str category { "seat" }; // seat | bed | workstation | grave | mount...
    str interaction;         // kFurnitureInteractions; "" = from category
    f32 restHours { 0.0f };  // hours slept/rested on Use; 0 = from category
    core::Guid model;        // 3D visual (StaticForm-like usage)
    core::Guid material;
    core::Guid sprite;       // 2D fallback
    core::Guid effect;       // EffectForm applied while in use (rest...)
    str screen;              // UI screen opened for the player ("" = none)
    // The ridden run speed
    // in m/s, read only for category "mount" (the tech-proof pony is
    // furniture with a procedural mesh; the REAL mount becomes an
    // ActorForm - followerCategory "mount" - once a horse rig exists).
    f32 mountSpeed { 9.0f };

    REFLECT_BEGIN(FurnitureForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(category)
        REFLECT_FIELD_ENUM(interaction, kFurnitureInteractions)
        REFLECT_FIELD(restHours)
        REFLECT_FIELD(model)
        REFLECT_FIELD(material)
        REFLECT_FIELD(sprite)
        REFLECT_FIELD(effect)
        REFLECT_FIELD(screen)
        REFLECT_FIELD(mountSpeed)
    REFLECT_END()
};

// One oriented use point (a bed has one, a bench several). Offset/facing
// are in the furniture's local space.
struct FurniturePointForm : data::Form {
    core::Guid parent; // FurnitureForm
    Vec3 offset { 0.0f, 0.0f, 0.0f };
    f32 facing { 0.0f };   // yaw radians in local space
    str animTag { "Sitting" }; // "State." + this = the anim gate

    REFLECT_BEGIN(FurniturePointForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(offset)
        REFLECT_FIELD(facing)
        REFLECT_FIELD(animTag)
    REFLECT_END()
};

void registerFurnitureFormTypes(data::FormTypeRegistry& registry);

// The interaction a piece of furniture offers: the parsed `interaction`
// when set, else derived from `category` (a category spelled like an
// interaction name — "grave", "mount", "container", "lever" — IS that
// interaction; any other category is Use).
FurnitureInteraction furnitureInteraction(const FurnitureForm& furniture);

// The hours a Use interaction skips: `restHours` when > 0, else 8 h for
// a "bed" category and 1 h (a nap on a seat) otherwise.
f32 furnitureRestHours(const FurnitureForm& furniture);

} // namespace gameplay
