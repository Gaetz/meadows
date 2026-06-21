#pragma once

#include <vector>

#include "data/forms/Form.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/Resonance.hpp"

// Drugs + harmony break (docs/STATS.md §2, N4): a drug applies a **sharp
// resonance boost** on one channel while active and **breaks Harmony** (the
// cascade is skipped — channels act independently, `Status.HarmonyBroken`); when
// it wears off it inflicts an **aftershock** — a negative resonance that recovers
// progressively at `aftershockRecoveryPerHour` per in-game hour back to 0.
// The aftershock is transient (not baked into persistent resonance) so that
// multiple drug aftershocks accumulate independently and are visible through
// `drugAftereffectResonance` separately from the active boost.

namespace data {
class FormTypeRegistry;
} // namespace data

namespace gameplay {

// A drug definition (§5 moddable). `channel` is onyx / amber / garnet.
struct DrugForm : data::Form {
    str displayName;
    str channel { "amber" };
    f32 resonanceBoost { 100.0f };         // transient boost on the channel while active
    f32 durationHours { 2.0f };
    f32 aftershockResonance { -30.0f };    // initial negative resonance on expiry
    f32 aftershockRecoveryPerHour { 1.0f }; // pts/game-hour toward 0; moddable

    REFLECT_BEGIN(DrugForm, data::Form)
        REFLECT_FIELD(displayName)
        REFLECT_FIELD(channel)
        REFLECT_FIELD(resonanceBoost)
        REFLECT_FIELD(durationHours)
        REFLECT_FIELD(aftershockResonance)
        REFLECT_FIELD(aftershockRecoveryPerHour)
    REFLECT_END()
};

struct ActiveDrug {
    str channel;
    f32 boost { 0.0f };
    f32 aftershock { 0.0f };
    f32 recoveryPerHour { 1.0f };
    f32 hoursRemaining { 0.0f };
    f32 totalHours { 0.0f };      // initial duration, kept for UI progress bar
};

// A progressive aftershock: created when a drug expires, decays toward 0 at
// recoveryPerHour pts/game-hour. Contributes to the effective resonance as a
// transient penalty separate from persistent resonance and survival needs.
struct DrugAftereffect {
    str channel;
    f32 remaining { 0.0f };         // negative; approaches 0 as it recovers
    f32 recoveryPerHour { 1.0f };
    f32 initialRemaining { 0.0f };  // initial value (negative), kept for UI progress bar
};

// Runtime component: the actor's active drugs and recovering aftereffects.
struct ActiveDrugs {
    std::vector<ActiveDrug>      list;
    std::vector<DrugAftereffect> aftereffects;
};

void registerDrugFormTypes(data::FormTypeRegistry& registry);

// Takes a drug: schedules its transient boost + aftershock and grants
// Status.HarmonyBroken (the high disables the harmony cascade).
void takeDrug(ActiveDrugs& drugs, const DrugForm& drug, AbilitySystem& system,
              const GameplayTagRegistry& tags);

// The combined transient boost from currently-active drugs (per channel).
Resonance drugResonance(const ActiveDrugs& drugs);

// The combined recovering aftershock resonance (per channel).
// Negative; fades toward 0 at recoveryPerHour per game-hour.
Resonance drugAftereffectResonance(const ActiveDrugs& drugs);

// Advances drugs by game-time. On expiry, creates a DrugAftereffect instead
// of writing to persistent resonance, and releases Status.HarmonyBroken.
// Also advances and removes completed aftereffects.
void tickDrugs(ActiveDrugs& drugs, AbilitySystem& system,
               f64 gameDt, const GameplayTagRegistry& tags);

// Whether the harmony cascade is currently broken (a drug is active).
bool harmonyBroken(const AbilitySystem& system, const GameplayTagRegistry& tags);

} // namespace gameplay
