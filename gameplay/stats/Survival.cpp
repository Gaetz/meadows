#include "gameplay/stats/Survival.hpp"

#include <algorithm>

#include "gameplay/ability/Attributes.hpp" // AttributeSet

namespace gameplay {

namespace {

static const char* kSurvivalAmberTag  = "Internal.SurvivalAmber";
static const char* kSurvivalGarnetTag = "Internal.SurvivalGarnet";

// Linear contribution: 0 at/above threshold, down to survivalResonanceAtEmpty at 0.
f32 needResonance(f32 value, const StatsTuningForm& t) {
    if (value >= t.survivalThreshold) {
        return 0.0f;
    }
    return (t.survivalThreshold - value) / t.survivalThreshold *
           t.survivalResonanceAtEmpty;
}

void applySurvivalEffect(const char* tagName, const char* attribute, f32 magnitude,
                         AbilitySystem& system, AttributeSet& vitals,
                         const GameplayTagRegistry& tags) {
    // Remove old effect for this slot.
    if (const auto t = tags.find(tagName)) {
        removeEffectsByGrantedTag(system, *t, tags);
    }
    if (magnitude == 0.0f) return;

    // Apply new infinite effect with current magnitude.
    EffectForm eff;
    eff.attribute = attribute;
    eff.op = "add";
    eff.magnitude = magnitude;
    eff.duration = "infinite";
    eff.grantedTag = tagName;
    applyEffect(vitals, system, eff, tags);
}

} // namespace

void tickSurvival(Survival& survival, f64 gameDt, const StatsTuningForm& tuning) {
    const f64 hours = gameDt / 3600.0;
    survival.hunger = std::max(
        0.0f, survival.hunger - static_cast<f32>(hours / tuning.hungerHoursPerPoint));
    survival.thirst = std::max(
        0.0f, survival.thirst - static_cast<f32>(hours / tuning.thirstHoursPerPoint));
    survival.sleep = std::max(
        0.0f, survival.sleep - static_cast<f32>(hours / tuning.sleepHoursPerPoint));
}

void updateSurvivalEffects(Survival& survival, AbilitySystem& system,
                           AttributeSet& vitals, const GameplayTagRegistry& tags,
                           const StatsTuningForm& tuning) {
    const f32 amberDelta = needResonance(survival.hunger, tuning)
                         + needResonance(survival.thirst, tuning);
    const f32 garnetDelta = needResonance(survival.sleep, tuning);

    applySurvivalEffect(kSurvivalAmberTag, "amber", amberDelta,
                        system, vitals, tags);
    applySurvivalEffect(kSurvivalGarnetTag, "garnet", garnetDelta,
                        system, vitals, tags);
}

void registerSurvivalTags(GameplayTagRegistry& tags) {
    tags.registerTag(kSurvivalAmberTag);
    tags.registerTag(kSurvivalGarnetTag);
}

} // namespace gameplay
