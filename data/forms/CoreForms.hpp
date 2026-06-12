#pragma once

#include "data/forms/Form.hpp"

// Early sample form types that exercise the data model. The real gameplay
// roster (containers, doors, effects, abilities...) lands with Phase 3;
// keep these two honest in the meantime.

namespace data {

class FormTypeRegistry;

struct WeaponForm : Form {
    str displayName;
    f32 damage { 10.0f };
    f32 weight { 1.0f };
    i32 goldValue { 0 };
    bool twoHanded { false };
    core::Guid sprite; // asset reference, resolved through the asset DB

    REFLECT_BEGIN(WeaponForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(damage)
        REFLECT_FIELD(weight)
        REFLECT_FIELD(goldValue)
        REFLECT_FIELD(twoHanded)
        REFLECT_FIELD(sprite)
    REFLECT_END()
};

struct ActorForm : Form {
    str displayName;
    f32 maxHealth { 100.0f };
    f32 walkSpeed { 3.0f };
    core::Guid sprite;

    REFLECT_BEGIN(ActorForm, Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(maxHealth)
        REFLECT_FIELD(walkSpeed)
        REFLECT_FIELD(sprite)
    REFLECT_END()
};

// Registers every core form type; call once at startup before loading
// plugins. Mods cannot add form *types* (§2.7) — richness comes from data
// on existing types, effects, and scripts.
void registerCoreFormTypes(FormTypeRegistry& registry);

} // namespace data
