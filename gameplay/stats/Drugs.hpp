#pragma once

#include <vector>

#include "data/forms/Form.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/Resonance.hpp"

// Drugs + harmony break (docs/STATS.md §2, N4): a drug applies a **sharp
// resonance boost** on one channel while active and **breaks Harmony** (the
// cascade is skipped — channels act independently, `Status.HarmonyBroken`); when
// it wears off it inflicts an **aftershock** (a negative resonance on that channel,
// applied to the persistent resonance, with harmony restored so it cascades).

namespace data {
class FormTypeRegistry;
} // namespace data

namespace gameplay {

// A drug definition (§5 moddable). `channel` is onyx / amber / garnet.
struct DrugForm : data::Form {
    str displayName;
    str channel { "amber" };
    f32 resonanceBoost { 100.0f };      // transient boost on the channel while active
    f32 durationHours { 2.0f };
    f32 aftershockResonance { -30.0f }; // applied to persistent resonance on expiry

    REFLECT_BEGIN(DrugForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(channel)
        REFLECT_FIELD(resonanceBoost)
        REFLECT_FIELD(durationHours)
        REFLECT_FIELD(aftershockResonance)
    REFLECT_END()
};

struct ActiveDrug {
    str channel;
    f32 boost { 0.0f };
    f32 aftershock { 0.0f };
    f32 hoursRemaining { 0.0f };
};

// Runtime component: the actor's active drugs.
struct ActiveDrugs {
    std::vector<ActiveDrug> list;
};

void registerDrugFormTypes(data::FormTypeRegistry& registry);

// Takes a drug: schedules its transient boost + aftershock and grants
// Status.HarmonyBroken (the high disables the harmony cascade).
void takeDrug(ActiveDrugs& drugs, const DrugForm& drug, AbilitySystem& system,
              const GameplayTagRegistry& tags);

// The combined transient boost from active drugs (per channel) — fold into the
// effective resonance.
Resonance drugResonance(const ActiveDrugs& drugs);

// Advances drugs by game-time; on expiry, applies the aftershock to `persistent`
// resonance and releases the actor's Status.HarmonyBroken hold.
void tickDrugs(ActiveDrugs& drugs, Resonance& persistent, AbilitySystem& system,
               f64 gameDt, const GameplayTagRegistry& tags);

// Whether the harmony cascade is currently broken (a drug is active).
bool harmonyBroken(const AbilitySystem& system, const GameplayTagRegistry& tags);

} // namespace gameplay
