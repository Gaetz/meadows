#pragma once

#include <vector>

#include "engine/core/Defines.hpp"
#include "engine/core/Rng.hpp"
#include "gameplay/ability/DerivedStats.hpp" // StatModifiers, attr

// Body-part injuries (docs/STATS.md §5, N2): bruise / cut / fracture, by body part,
// each carrying an onyx resonance penalty + per-body-part attribute & leg-speed
// maluses + a rest-driven recovery timer. Inflict is gated by resonance-resistance
// (§2) and rolled via the engine RNG (§8). open/infected sub-states, aggravation,
// and treatment items are a follow-up (`[7+]`).

namespace gameplay {

enum class InjuryType { Bruise, Cut, Fracture };
enum class BodyPart { Head, Torso, Arms, Legs };

struct Injury {
    InjuryType type { InjuryType::Bruise };
    BodyPart part { BodyPart::Torso };
    i32 severity { 0 }; // 0 = light; cut/fracture up to 2 (severe); bruise up to 1
    f32 recoveryHoursRemaining { 0.0f };
};

// Runtime component (a container, like Inventory): the actor's active injuries.
// Serialization is Phase 8.
struct Injuries {
    std::vector<Injury> list;
};

// The onyx (health) resonance penalty from all injuries (≤ 0); fold into the
// effective resonance, so it also resists further injuries (§2).
f32 injuryResonance(const Injuries& injuries);

// Folds every injury's attribute and leg-speed maluses into `mods`.
void injuryStatModifiers(const Injuries& injuries, StatModifiers& mods);

// Adds an injury at a body part, or aggravates an existing one of the same type
// (severity +1, capped); (re)sets the recovery timer.
void addInjury(Injuries& injuries, InjuryType type, BodyPart part);

// The base inflict chance from the fraction of max-health a hit removed (a rough
// model of the design's per-type source thresholds).
f64 injuryBaseChance(InjuryType type, f32 healthFractionRemoved);

// Rolls an injury: immune at non-negative onyx; otherwise `baseChance × |onyx|/100`
// through the RNG (§8). Returns whether it was inflicted.
bool rollInjury(Injuries& injuries, InjuryType type, BodyPart part, f64 baseChance,
                f32 onyxResonance, core::Rng& rng);

// Recovers injuries over `restHours` of Rest: counts each timer down; when a rank
// elapses the severity drops one (the injury clears below light).
void recoverInjuries(Injuries& injuries, f32 restHours);

} // namespace gameplay
