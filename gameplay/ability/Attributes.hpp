#pragma once

#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/core/Hash.hpp"
#include "engine/reflect/Reflect.hpp"

// Attributes (§6). An AttributeSet is a reflected component whose f32 fields are
// the **BaseValues** — patchable like any Form field (§5) and serializable
// (save layer). The matching **CurrentValues** live in a runtime overlay on the
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
    f32 energy { 100.0f };
    f32 maxEnergy { 100.0f };
    f32 essence { 50.0f };
    f32 maxEssence { 50.0f };
    f32 armorRating { 0.0f };
    f32 damage { 0.0f }; // meta-attribute (transient)
    // The STATS.md balance override (`derived value = override ?? formula`,
    // decided Phase 6): > 0 pins maxHealth past the humanoid attribute
    // formula — the Spawner seeds it from ActorForm.maxHealth (a bandit
    // really has its authored 35). 0 = the formula (the Player: his max
    // progresses with attributes). Resonance's % still applies after.
    // Appended (binary ordinals stable).
    f32 maxHealthOverride { 0.0f };

    REFLECT_BEGIN(AttributeSet, void)
        REFLECT_FIELD(health)
        REFLECT_FIELD(maxHealth)
        REFLECT_FIELD(energy)
        REFLECT_FIELD(maxEnergy)
        REFLECT_FIELD(essence)
        REFLECT_FIELD(maxEssence)
        REFLECT_FIELD(armorRating)
        REFLECT_FIELD(damage)
        REFLECT_FIELD(maxHealthOverride)
    REFLECT_END()
};

// The data-driven handle for an attribute = fnv1a of its field name.
inline u32 attr(std::string_view field) { return core::fnv1a(field); }

} // namespace gameplay
