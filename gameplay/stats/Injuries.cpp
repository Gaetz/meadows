#include "gameplay/stats/Injuries.hpp"

#include <algorithm>

#include "gameplay/ability/Attributes.hpp" // AttributeSet

namespace gameplay {

namespace {

static const char* kInjuryActiveTag = "Injury.Active";

constexpr f32 kCutResonance[3] = { -1.0f, -2.0f, -4.0f };
constexpr f32 kFractureResonance[3] = { -10.0f, -20.0f, -30.0f };

i32 clampSeverity(i32 sev) { return std::clamp(sev, 0, 2); }

f32 resonancePenalty(InjuryType type, i32 sev) {
    sev = clampSeverity(sev);
    switch (type) {
    case InjuryType::Bruise:   return sev == 0 ? -1.0f : -2.0f;
    case InjuryType::Cut:      return kCutResonance[sev];
    case InjuryType::Fracture: return kFractureResonance[sev];
    }
    return 0.0f;
}

struct AttrMalus {
    const char* attribute { nullptr };
    f32 value { 0.0f };
};

AttrMalus attributeMalus(InjuryType type, BodyPart part, i32 sev) {
    sev = clampSeverity(sev);
    switch (type) {
    case InjuryType::Bruise: {
        const f32 v = sev == 0 ? 0.0f : -1.0f;
        switch (part) {
        case BodyPart::Head:  return { "grace", v };
        case BodyPart::Torso: return { "strength", v };
        case BodyPart::Arms:  return { "dexterity", v };
        case BodyPart::Legs:  return {}; // speed only
        }
    } break;
    case InjuryType::Cut: {
        static constexpr f32 head[3] = { -1, -2, -3 };
        static constexpr f32 torso[3] = { -1, -2, -3 };
        static constexpr f32 arms[3] = { 0, -1, -2 };
        static constexpr f32 legs[3] = { -1, -2, -3 };
        switch (part) {
        case BodyPart::Head:  return { "alacrity", head[sev] };
        case BodyPart::Torso: return { "constitution", torso[sev] };
        case BodyPart::Arms:  return { "dexterity", arms[sev] };
        case BodyPart::Legs:  return { "strength", legs[sev] };
        }
    } break;
    case InjuryType::Fracture: {
        static constexpr f32 head[3] = { -1, -3, -4 };
        static constexpr f32 torso[3] = { -1, -3, -4 };
        static constexpr f32 arms[3] = { -1, -2, -3 };
        static constexpr f32 legs[3] = { -1, -2, -3 };
        switch (part) {
        case BodyPart::Head:  return { "alacrity", head[sev] };
        case BodyPart::Torso: return { "ego", torso[sev] };
        case BodyPart::Arms:  return { "dexterity", arms[sev] };
        case BodyPart::Legs:  return { "strength", legs[sev] };
        }
    } break;
    }
    return {};
}

f32 speedMalusPercent(InjuryType type, BodyPart part, i32 sev) {
    if (part != BodyPart::Legs) {
        return 0.0f;
    }
    sev = clampSeverity(sev);
    switch (type) {
    case InjuryType::Bruise:   return sev == 0 ? 0.0f : -5.0f;
    case InjuryType::Cut: {
        static constexpr f32 s[3] = { 0, -5, -10 };
        return s[sev];
    }
    case InjuryType::Fracture: {
        static constexpr f32 s[3] = { -10, -25, -40 };
        return s[sev];
    }
    }
    return 0.0f;
}

i32 maxSeverity(InjuryType type) { return type == InjuryType::Bruise ? 1 : 2; }

f32 recoveryHours(InjuryType type) {
    switch (type) {
    case InjuryType::Bruise:   return 24.0f;
    case InjuryType::Cut:      return 48.0f;
    case InjuryType::Fracture: return 72.0f;
    }
    return 24.0f;
}

void applyInjuryEffect(const Injury& inj, AbilitySystem& system,
                       AttributeSet& vitals, const GameplayTagRegistry& tags) {
    const f32 res = resonancePenalty(inj.type, inj.severity);
    // Primary: onyx resonance penalty.
    {
        EffectForm eff;
        eff.attribute = "onyx";
        eff.op = "add";
        eff.magnitude = res;
        eff.duration = "infinite";
        eff.grantedTag = kInjuryActiveTag;
        applyEffect(vitals, system, eff, tags);
    }
    // Secondary: attribute malus (if any).
    const AttrMalus malus = attributeMalus(inj.type, inj.part, inj.severity);
    if (malus.attribute && malus.value != 0.0f) {
        EffectForm eff;
        eff.attribute = malus.attribute;
        eff.op = "add";
        eff.magnitude = malus.value;
        eff.duration = "infinite";
        eff.grantedTag = kInjuryActiveTag;
        applyEffect(vitals, system, eff, tags);
    }
    // Tertiary: movement speed multiplier (legs only).
    const f32 speed = speedMalusPercent(inj.type, inj.part, inj.severity);
    if (speed != 0.0f) {
        EffectForm eff;
        eff.attribute = "movementSpeed";
        eff.op = "multiply";
        eff.magnitude = 1.0f + speed / 100.0f;
        eff.duration = "infinite";
        eff.grantedTag = kInjuryActiveTag;
        applyEffect(vitals, system, eff, tags);
    }
}

} // namespace

void addInjury(Injuries& injuries, InjuryType type, BodyPart part) {
    for (Injury& inj : injuries.list) {
        if (inj.type == type && inj.part == part) {
            inj.severity = std::min(inj.severity + 1, maxSeverity(type));
            inj.recoveryHoursRemaining = recoveryHours(type);
            return;
        }
    }
    injuries.list.push_back({ type, part, 0, recoveryHours(type) });
}

void syncInjuryEffects(const Injuries& injuries, AbilitySystem& system,
                       AttributeSet& vitals, const GameplayTagRegistry& tags) {
    // Remove all existing injury effects.
    if (const auto t = tags.find(kInjuryActiveTag)) {
        removeEffectsByGrantedTag(system, *t, tags);
    }
    // Re-apply for current state.
    for (const Injury& inj : injuries.list) {
        applyInjuryEffect(inj, system, vitals, tags);
    }
}

f64 injuryBaseChance(InjuryType type, f32 healthFractionRemoved) {
    const f64 h = static_cast<f64>(healthFractionRemoved);
    switch (type) {
    case InjuryType::Bruise:   return h > 0.05 ? h : 0.0;
    case InjuryType::Cut:      return h > 0.10 ? h : 0.0;
    case InjuryType::Fracture: return h > 0.50 ? 0.5 : 0.0;
    }
    return 0.0;
}

bool rollInjury(Injuries& injuries, InjuryType type, BodyPart part, f64 baseChance,
                f32 onyxResonance, core::Rng& rng) {
    if (onyxResonance >= 0.0f) {
        return false; // resonance resistance: cannot be injured (§2)
    }
    const f64 chance = baseChance * (-static_cast<f64>(onyxResonance) / 100.0);
    if (!rng.chance(chance)) {
        return false;
    }
    addInjury(injuries, type, part);
    return true;
}

void recoverInjuries(Injuries& injuries, f32 restHours) {
    for (Injury& inj : injuries.list) {
        inj.recoveryHoursRemaining -= restHours;
        while (inj.recoveryHoursRemaining <= 0.0f && inj.severity >= 0) {
            inj.severity -= 1;
            if (inj.severity < 0) {
                break;
            }
            inj.recoveryHoursRemaining += recoveryHours(inj.type);
        }
    }
    std::erase_if(injuries.list,
                  [](const Injury& inj) { return inj.severity < 0; });
}

void registerInjuryTags(GameplayTagRegistry& tags) {
    tags.registerTag(kInjuryActiveTag);
}

} // namespace gameplay
