#pragma once

#include "data/forms/Form.hpp"

// Furniture & workstations (horizontal pass H1) — the SHARED player/NPC
// object-interaction system: beds, chairs, forges, alchemy tables. What
// makes NPC schedules VISIBLE. A furniture piece is a placeable base form
// (Furniture spawn category); its use points are child records.
//
// HOW TO FILL (post-7/07): the Furniture vertical (H7 skeleton has the
// occupancy API) walks the user to the point, plays `animTag` through the
// anim graph, applies `effect` (GAS) while used — sleeping IS the Phase-7
// rest effect, crafting stations open their UI screen for the player.

namespace data {
class FormTypeRegistry;
}

namespace gameplay {

struct FurnitureForm : data::Form {
    str displayName;
    str category { "seat" }; // seat | bed | workstation | container-like
    core::Guid model;        // 3D visual (StaticForm-like usage)
    core::Guid material;
    core::Guid sprite;       // 2D fallback
    core::Guid effect;       // EffectForm applied while in use (rest...)
    str screen;              // UI screen opened for the player ("" = none)

    REFLECT_BEGIN(FurnitureForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(category)
        REFLECT_FIELD(model)
        REFLECT_FIELD(material)
        REFLECT_FIELD(sprite)
        REFLECT_FIELD(effect)
        REFLECT_FIELD(screen)
    REFLECT_END()
};

// One oriented use point (a bed has one, a bench several). Offset/facing
// are in the furniture's local space.
struct FurniturePointForm : data::Form {
    core::Guid parent; // FurnitureForm
    Vec3 offset { 0.0f, 0.0f, 0.0f };
    f32 facing { 0.0f };   // yaw radians in local space
    str animTag { "Sit" }; // anim-graph tag gate while used

    REFLECT_BEGIN(FurniturePointForm, data::Form)
        REFLECT_FIELD(parent)
        REFLECT_FIELD(offset)
        REFLECT_FIELD(facing)
        REFLECT_FIELD(animTag)
    REFLECT_END()
};

void registerFurnitureFormTypes(data::FormTypeRegistry& registry);

} // namespace gameplay
