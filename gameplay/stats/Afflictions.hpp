#pragma once

#include <vector>

#include "data/forms/Form.hpp"
#include "engine/core/Rng.hpp"
#include "gameplay/ability/DerivedStats.hpp" // StatModifiers, attr
#include "gameplay/stats/Resonance.hpp"

// Diseases & psychoses (docs/STATS.md §5, N3): the same permanent-status lifecycle
// as injuries (N2) — a resonance penalty + an attribute malus + rest-driven
// recovery, inflicted gated by resonance-resistance (§2) via the RNG (§8) — but on
// the **energy** (amber, diseases) and **essence** (garnet, psychoses) channels,
// and **data-defined** (an AfflictionForm, not the body-part C++ tables of
// injuries). Inflicted by a status buildup trigger (N1) or events.

namespace data {
class FormDatabase;
class FormTypeRegistry;
} // namespace data

namespace gameplay {

// A disease or psychosis definition (§5 moddable). `channel` is "amber" (disease)
// or "garnet" (psychosis).
struct AfflictionForm : data::Form {
    str displayName;
    str channel { "amber" };
    f32 resonancePenalty { -10.0f }; // added to the channel while afflicted (≤ 0)
    str attributeMalus;              // optional attribute weakened while afflicted
    f32 attributeMalusValue { 0.0f };
    f32 recoveryHours { 48.0f };

    REFLECT_BEGIN(AfflictionForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(channel)
        REFLECT_FIELD(resonancePenalty)
        REFLECT_FIELD(attributeMalus)
        REFLECT_FIELD(attributeMalusValue)
        REFLECT_FIELD(recoveryHours)
    REFLECT_END()
};

struct ActiveAffliction {
    core::Guid form;
    f32 recoveryHoursRemaining { 0.0f };
};

// Runtime component (a container, like Injuries): the actor's active afflictions.
struct Afflictions {
    std::vector<ActiveAffliction> list;
};

// Registers the AfflictionForm type (called by registerStatsFormTypes).
void registerAfflictionFormTypes(data::FormTypeRegistry& registry);

// The channel resonance penalties from all afflictions (amber + garnet; onyx 0),
// to fold into the effective resonance.
Resonance afflictionResonance(const Afflictions& afflictions,
                              const data::FormDatabase& forms);

// The attribute maluses from all afflictions.
void afflictionStatModifiers(const Afflictions& afflictions,
                             const data::FormDatabase& forms, StatModifiers& mods);

// Inflicts an affliction: immune at non-negative channel resonance; otherwise
// `baseChance × |channelResonance|/100` through the RNG (§8). Re-inflicting resets
// the recovery timer. Returns whether it was inflicted.
bool inflictAffliction(Afflictions& afflictions, const core::Guid& form,
                       const AfflictionForm& definition, f32 channelResonance,
                       f64 baseChance, core::Rng& rng);

// Recovers afflictions over `restHours` of Rest: clears each when its timer elapses.
void recoverAfflictions(Afflictions& afflictions, f32 restHours);

} // namespace gameplay
