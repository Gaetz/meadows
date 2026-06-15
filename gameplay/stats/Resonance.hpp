#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/ability/DerivedStats.hpp" // StatModifiers

// Resonance (docs/STATS.md §2): the hidden, signed (de)synchronization with the
// three spheres. onyx↔health, amber↔energy, garnet↔essence. Range -100..1500
// (negative = dissonance). A reflected component (serializes via §5). It feeds
// the derived pass through StatModifiers — it never sets attributes directly (§2.9).

namespace gameplay {

struct Resonance {
    f32 onyx { 0.0f };   // health channel
    f32 amber { 0.0f };  // energy channel
    f32 garnet { 0.0f }; // essence channel

    REFLECT_BEGIN(Resonance, void)
        REFLECT_FIELD(onyx)
        REFLECT_FIELD(amber)
        REFLECT_FIELD(garnet)
    REFLECT_END()
};

// Harmony (docs/STATS.md §2): channels are coupled. From the most-displaced
// channel, a displacement cascades **half** then **quarter** (truncated) to the
// next two channels in the cycle amber→garnet→onyx, **once**, as a *minimum*
// displacement (the channel keeps its own value if already more displaced).
// Returns the effective per-channel resonance after the cascade.
Resonance harmonyEffective(const Resonance& res);

// Translates resonance into stat modifiers (after the harmony cascade): each
// channel's effective resonance `r` scales its primary max by (1 + r/100) and
// offsets its three linked attributes by trunc(r/15). Pass the result to
// recomputeCurrent as its `extra` modifiers.
void buildResonanceModifiers(const Resonance& res, StatModifiers& mods);

} // namespace gameplay
