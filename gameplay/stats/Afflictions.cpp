#include "gameplay/stats/Afflictions.hpp"

namespace gameplay {

bool inflictEffect(AttributeSet& vitals, AbilitySystem& system,
                   const EffectForm& effect, f32 channelResonance,
                   f64 baseChance, core::Rng& rng,
                   const GameplayTagRegistry& tags) {
    if (channelResonance >= 0.0f) {
        return false; // resonance resistance: cannot be afflicted (§2)
    }
    const f64 chance = baseChance * (-static_cast<f64>(channelResonance) / 100.0);
    if (!rng.chance(chance)) {
        return false;
    }
    // Re-infliction refreshes the timer: remove any existing effect with the same
    // grantedTag, then apply fresh.
    if (!effect.grantedTag.empty()) {
        if (const auto tag = tags.find(effect.grantedTag)) {
            removeEffectsByGrantedTag(system, *tag, tags);
        }
    }
    applyEffect(vitals, system, effect, tags);
    return true;
}

} // namespace gameplay
