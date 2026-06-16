#include "gameplay/stats/Injuries.hpp"

#include <algorithm>

namespace gameplay {

namespace {
// docs/STATS.md §5 tables, baked in C++ (the design's specific values; §2.7).
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
} // namespace

f32 injuryResonance(const Injuries& injuries) {
    f32 total = 0.0f;
    for (const Injury& inj : injuries.list) {
        total += resonancePenalty(inj.type, inj.severity);
    }
    return total;
}

void injuryStatModifiers(const Injuries& injuries, StatModifiers& mods) {
    for (const Injury& inj : injuries.list) {
        const AttrMalus malus = attributeMalus(inj.type, inj.part, inj.severity);
        if (malus.attribute && malus.value != 0.0f) {
            mods.add[attr(malus.attribute)] += malus.value;
        }
        const f32 speed = speedMalusPercent(inj.type, inj.part, inj.severity);
        if (speed != 0.0f) {
            auto [it, inserted] = mods.mul.try_emplace(attr("movementSpeed"), 1.0f);
            it->second *= (1.0f + speed / 100.0f);
        }
    }
}

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

} // namespace gameplay
