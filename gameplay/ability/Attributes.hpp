#pragma once

#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/core/Hash.hpp"
#include "engine/reflect/Reflect.hpp"

// Attributes (§6). An AttributeSet is a reflected component whose f32 fields are
// the **BaseValues** — patchable like any Form field (§5) and serializable
// (Phase 5). The matching **CurrentValues** live in a runtime overlay on the
// AbilitySystem, recomputed from base + active modifiers (§2.9). An attribute is
// addressed by the fnv1a id of its field name, resolved through reflection
// (`TypeInfo::findField`) — the same idea as UE's FGameplayAttribute (a
// reflected property reference). Effects (brick 3c) target attributes by that id.

namespace gameplay {

// The core attribute set (Vitals). `damage` is a transient meta-attribute:
// effects write it and a PostExecute hook (3c) routes it into health, keeping
// the damage formula data-driven.
struct AttributeSet {
    f32 health { 100.0f };
    f32 maxHealth { 100.0f };
    f32 stamina { 100.0f };
    f32 maxStamina { 100.0f };
    f32 magicka { 50.0f };
    f32 maxMagicka { 50.0f };
    f32 armorRating { 0.0f };
    f32 damage { 0.0f }; // meta-attribute (transient)

    REFLECT_BEGIN(AttributeSet, void)
        REFLECT_FIELD(health)
        REFLECT_FIELD(maxHealth)
        REFLECT_FIELD(stamina)
        REFLECT_FIELD(maxStamina)
        REFLECT_FIELD(magicka)
        REFLECT_FIELD(maxMagicka)
        REFLECT_FIELD(armorRating)
        REFLECT_FIELD(damage)
    REFLECT_END()
};

// The data-driven handle for an attribute = fnv1a of its field name.
inline u32 attr(std::string_view field) { return core::fnv1a(field); }

} // namespace gameplay
