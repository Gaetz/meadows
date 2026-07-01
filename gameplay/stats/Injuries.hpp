#pragma once

#include <vector>

#include "engine/core/Defines.hpp"
#include "engine/core/Rng.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/DerivedStats.hpp" // StatModifiers, attr
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"

// Body-part injuries (docs/STATS.md §5, N2): bruise / cut / fracture, by body part,
// each carrying an onyx resonance penalty + per-body-part attribute & leg-speed
// maluses via GAS effects (gameTime = true, durationHours = recoveryHours).
// Inflict is gated by resonance-resistance (§2) and rolled via the engine RNG (§8).

namespace gameplay {

struct AttributeSet; // from Attributes.hpp

enum class InjuryType { Bruise, Cut, Fracture };
enum class BodyPart { Head, Torso, Arms, Legs };

struct Injury {
    InjuryType type { InjuryType::Bruise };
    BodyPart part { BodyPart::Torso };
    i32 severity { 0 }; // 0 = light; cut/fracture up to 2 (severe); bruise up to 1
    f32 recoveryHoursRemaining { 0.0f };
};

// Runtime component (a container, like Inventory): the actor's active injuries.
struct Injuries {
    std::vector<Injury> list;
};

// Adds an injury at a body part, or aggravates an existing one of the same type
// (severity +1, capped); (re)sets the recovery timer.
void addInjury(Injuries& injuries, InjuryType type, BodyPart part);

// Syncs all injury GAS effects to match the current Injuries list.
// Removes all "Injury.Active"-tagged effects, then re-applies effects for each
// current injury (onyx penalty + attribute malus + speed malus via EffectForm).
// Call after addInjury() and after recoverInjuries().
void syncInjuryEffects(const Injuries& injuries, AbilitySystem& system,
                       AttributeSet& vitals, const GameplayTagRegistry& tags);

// The base inflict chance from the fraction of max-health a hit removed (a rough
// model of the design's per-type source thresholds).
f64 injuryBaseChance(InjuryType type, f32 healthFractionRemoved);

// Rolls an injury: immune at non-negative onyx; otherwise `baseChance × |onyx|/100`
// through the RNG (§8). Returns whether it was inflicted. Does NOT call
// syncInjuryEffects — caller must do so after this returns true.
bool rollInjury(Injuries& injuries, InjuryType type, BodyPart part, f64 baseChance,
                f32 onyxResonance, core::Rng& rng);

// Recovers injuries over `restHours` of Rest: counts each timer down; when a rank
// elapses the severity drops one (the injury clears below light).
void recoverInjuries(Injuries& injuries, f32 restHours);

// Pre-register the internal injury effect tag. Call once at startup.
void registerInjuryTags(GameplayTagRegistry& tags);

} // namespace gameplay
