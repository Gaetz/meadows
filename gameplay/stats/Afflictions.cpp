#include "gameplay/stats/Afflictions.hpp"

#include <algorithm>
#include <string_view>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

void registerAfflictionFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<AfflictionForm>();
}

Resonance afflictionResonance(const Afflictions& afflictions,
                              const data::FormDatabase& forms) {
    Resonance result; // onyx stays 0; afflictions hit amber / garnet
    for (const ActiveAffliction& active : afflictions.list) {
        const AfflictionForm* def = forms.find<AfflictionForm>(active.form);
        if (!def) {
            continue;
        }
        if (def->channel == "garnet") {
            result.garnet += def->resonancePenalty;
        } else {
            result.amber += def->resonancePenalty; // "amber" (default)
        }
    }
    return result;
}

void afflictionStatModifiers(const Afflictions& afflictions,
                             const data::FormDatabase& forms, StatModifiers& mods) {
    for (const ActiveAffliction& active : afflictions.list) {
        const AfflictionForm* def = forms.find<AfflictionForm>(active.form);
        if (def && !def->attributeMalus.empty() && def->attributeMalusValue != 0.0f) {
            mods.add[attr(def->attributeMalus)] += def->attributeMalusValue;
        }
    }
}

bool inflictAffliction(Afflictions& afflictions, const core::Guid& form,
                       const AfflictionForm& definition, f32 channelResonance,
                       f64 baseChance, core::Rng& rng) {
    if (channelResonance >= 0.0f) {
        return false; // resonance resistance: cannot be afflicted (§2)
    }
    const f64 chance = baseChance * (-static_cast<f64>(channelResonance) / 100.0);
    if (!rng.chance(chance)) {
        return false;
    }
    for (ActiveAffliction& active : afflictions.list) {
        if (active.form == form) {
            active.recoveryHoursRemaining = definition.recoveryHours; // refresh
            return true;
        }
    }
    afflictions.list.push_back({ form, definition.recoveryHours });
    return true;
}

void recoverAfflictions(Afflictions& afflictions, f32 restHours) {
    for (ActiveAffliction& active : afflictions.list) {
        active.recoveryHoursRemaining -= restHours;
    }
    std::erase_if(afflictions.list, [](const ActiveAffliction& active) {
        return active.recoveryHoursRemaining <= 0.0f;
    });
}

} // namespace gameplay
