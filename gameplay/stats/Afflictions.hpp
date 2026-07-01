#pragma once

#include "engine/core/Rng.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/Resonance.hpp"

// Afflictions (diseases / psychoses) are now expressed directly as EffectForms
// routed through the GAS: the resonance penalty and attribute malus are active
// effects in the AbilitySystem (duration = recoveryHours × 3600 game-seconds,
// gameTime = true). This file retains the gate logic (resonance-resistance + RNG)
// for infliction and the form-type registration for legacy data compatibility.

namespace data {
class FormDatabase;
class FormTypeRegistry;
} // namespace data

namespace gameplay {

// Applies a resonance-gated, RNG-rolled effect to the target.
// channelResonance >= 0 → immune (§2). Otherwise the chance is
// baseChance × |channelResonance| / 100. If successful, calls applyEffect().
// The EffectForm must have grantedTag set (used for re-infliction refresh:
// remove existing effect with that tag, then apply fresh). Returns true if inflicted.
bool inflictEffect(AttributeSet& vitals, AbilitySystem& system,
                   const EffectForm& effect, f32 channelResonance,
                   f64 baseChance, core::Rng& rng,
                   const GameplayTagRegistry& tags);

} // namespace gameplay
