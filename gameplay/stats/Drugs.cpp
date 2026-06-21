#include "gameplay/stats/Drugs.hpp"

#include <algorithm>

#include "data/forms/FormTypeRegistry.hpp"

namespace gameplay {

namespace {
const char* kHarmonyBroken = "Status.HarmonyBroken";

// Adds `amount` to the channel named `channel` of a Resonance.
void addToChannel(Resonance& res, const str& channel, f32 amount) {
    if (channel == "onyx") {
        res.onyx += amount;
    } else if (channel == "garnet") {
        res.garnet += amount;
    } else {
        res.amber += amount; // "amber" (default)
    }
}
} // namespace

void registerDrugFormTypes(data::FormTypeRegistry& registry) {
    registry.registerFormType<DrugForm>();
}

void takeDrug(ActiveDrugs& drugs, const DrugForm& drug, AbilitySystem& system,
              const GameplayTagRegistry& tags) {
    drugs.list.push_back(
        { drug.channel, drug.resonanceBoost, drug.aftershockResonance,
          drug.aftershockRecoveryPerHour, drug.durationHours, drug.durationHours });
    if (const auto tag = tags.find(kHarmonyBroken)) {
        system.tags.add(*tag, tags); // ref-counted; one hold per active drug
    }
}

Resonance drugResonance(const ActiveDrugs& drugs) {
    Resonance result;
    for (const ActiveDrug& drug : drugs.list) {
        addToChannel(result, drug.channel, drug.boost);
    }
    return result;
}

Resonance drugAftereffectResonance(const ActiveDrugs& drugs) {
    Resonance result;
    for (const DrugAftereffect& ae : drugs.aftereffects) {
        addToChannel(result, ae.channel, ae.remaining);
    }
    return result;
}

void tickDrugs(ActiveDrugs& drugs, AbilitySystem& system,
               f64 gameDt, const GameplayTagRegistry& tags) {
    const f32 hours = static_cast<f32>(gameDt / 3600.0);
    const auto tag = tags.find(kHarmonyBroken);

    // Step 1: advance existing aftereffects (before expiry, so new ones don't
    // recover in the same tick they are created).
    for (DrugAftereffect& ae : drugs.aftereffects) {
        ae.remaining = std::min(0.0f, ae.remaining + ae.recoveryPerHour * hours);
    }
    std::erase_if(drugs.aftereffects,
                  [](const DrugAftereffect& ae) { return ae.remaining >= 0.0f; });

    // Step 2: expire drugs; spawn new aftereffects (they recover from next tick).
    for (ActiveDrug& drug : drugs.list) {
        drug.hoursRemaining -= hours;
        if (drug.hoursRemaining <= 0.0f) {
            // Aftershock becomes a progressive aftereffect — not baked into
            // persistent resonance. Harmony is restored (hold released) so the
            // cascade will apply to the aftereffect on the next recompute.
            drugs.aftereffects.push_back(
                { drug.channel, drug.aftershock, drug.recoveryPerHour, drug.aftershock });
            if (tag) {
                system.tags.remove(*tag, tags);
            }
        }
    }
    std::erase_if(drugs.list,
                  [](const ActiveDrug& d) { return d.hoursRemaining <= 0.0f; });
}

bool harmonyBroken(const AbilitySystem& system, const GameplayTagRegistry& tags) {
    const auto tag = tags.find(kHarmonyBroken);
    return tag && system.tags.has(*tag);
}

} // namespace gameplay
