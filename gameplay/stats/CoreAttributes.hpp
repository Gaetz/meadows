#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

// The nine character attributes (docs/STATS.md §1), grouped three-per-primary:
//   health  ← strength, constitution, grace
//   energy  ← dexterity, alacrity, perception
//   essence ← charisma, ego, insight
// A reflected AttributeSet (§6), like Vitals (gameplay/ability/Attributes.hpp):
// its f32 fields are BaseValues; CurrentValues live in the AbilitySystem overlay
// and feed the derived-stat pass (gameplay/ability/DerivedStats). Field names are
// globally unique across sets, so the overlay (keyed by fnv1a of the name) never
// collides. Defaults: 6, except insight (starts at 0).

namespace gameplay {

struct CoreAttributes {
    f32 strength { 6.0f };
    f32 constitution { 6.0f };
    f32 grace { 6.0f };
    f32 dexterity { 6.0f };
    f32 alacrity { 6.0f };
    f32 perception { 6.0f };
    f32 charisma { 6.0f };
    f32 ego { 6.0f };
    f32 insight { 0.0f };

    REFLECT_BEGIN(CoreAttributes, void)
        REFLECT_FIELD(strength)
        REFLECT_FIELD(constitution)
        REFLECT_FIELD(grace)
        REFLECT_FIELD(dexterity)
        REFLECT_FIELD(alacrity)
        REFLECT_FIELD(perception)
        REFLECT_FIELD(charisma)
        REFLECT_FIELD(ego)
        REFLECT_FIELD(insight)
    REFLECT_END()
};

} // namespace gameplay
