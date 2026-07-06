#pragma once

#include "data/forms/Form.hpp"
#include "gameplay/ability/GameplayTags.hpp"

// Moddable tuning constants for the stats system (§5). A single Form, resolved
// from the FormDatabase by a canonical guid; the defaults below match the
// original hard-coded values, so behaviour is unchanged when no tuning Form is
// present. New stats content (Phase 7) reads its knobs from here too. Threaded
// into the derived calculators (captured), survival, and damage.

namespace data {
class FormDatabase;
class FormTypeRegistry;
} // namespace data

namespace gameplay {

struct StatsTuningForm : data::Form {
    // Derived stats (CharacterStats).
    f32 attributeToMax { 5.0f };          // primary max = Σ(linked attr) × this
    f32 mitigationPerAttribute { 0.5f };  // defense / armor / resistance per attr
    f32 willBase { 1.0f };
    f32 willPerEgo { 0.25f };
    f32 basePosture { 50.0f };
    f32 posturePerAlacrity { 1.0f };
    f32 postureRegenBase { 2.0f };
    f32 postureRegenPerAlacrity { 0.333333f };
    f32 energyRegenBase { 35.0f };      // energy/s base (docs/STATS.md §3)
    f32 energyRegenPerAlacrity { 1.0f };
    f32 healthRegenPerGrace { 0.0002f };    // HP/s per grace (very slow; food raises it)
    f32 essenceRegenBase { 0.01f };         // essence/s base (docs/STATS.md §3)
    f32 essenceRegenPerInsight { 0.0025f }; // essence/s per insight
    f32 critSensBase { 25.0f };
    f32 critSensPerConstitution { 0.1f }; // subtracted
    // Status buildup / endurance (StatusBuildup, N1).
    f32 enduranceBase { 100.0f };            // buildup threshold to trigger a status
    f32 endurancePerAttribute { 0.5f };
    f32 statusBuildupDecayFlat { 3.0f };     // points/s before status acquired
    f32 statusBuildupDecayPercent { 0.01f }; // fraction/s after status acquired (1%/s)
    // Status effect magnitudes (all moddable via §5 patch layer).
    f32 poisonBaseDamagePerSecond { 1.0f };        // HP/s base while poisoned (vitality-reduced)
    f32 ignitionDamagePercent { 0.002f };          // fraction of maxHealth lost/s while ignited
    f32 electrocutionDamagePercent { 0.002f };     // fraction of maxEssence lost/s while electrocuted
    f32 electrocutionPostureDrainPercent { 1.0f }; // fraction of maxPosture removed on trigger
    f32 glaciationParalysisDuration { 3.0f };      // paralysis seconds on glaciation trigger
    f32 glaciationEnergyRegenMult { 0.7f };        // energy regen multiplier while glaciated
    f32 bleedBurstDamage { 30.0f };                // slash damage dealt on bleed trigger
    // Rest (Rest.cpp).
    f32 comfortableSleepHours { 8.0f };  // hours for a full-rest sleep (restores sleep to 100)
    f32 sleepPerHour { 2.0f };           // sleep points recovered per hour below comfortable
    // Damage (Damage).
    f32 flatMitigationCapBase { 25.0f };  // flat reduction cap = (this + attr) %
    f32 staggerSeconds { 1.5f };
    // Survival (Survival).
    f32 survivalThreshold { 75.0f };      // below this, a need drives resonance
    f32 survivalResonanceAtEmpty { -50.0f };
    f32 hungerHoursPerPoint { 0.96f };   // 1.042 pts/game_h → threshold (75) in 24h from 100
    f32 thirstHoursPerPoint { 0.32f };   // 3.125 pts/game_h → threshold (75) in 8h from 100
    f32 sleepHoursPerPoint { 0.72f };    // 1.389 pts/game_h → threshold (75) in 18h from 100
    // Barter (chantier 4, appended — ordinals stable). Prices = goldValue
    // × these; charisma/speechcraft scaling joins with the P1 stats pass.
    f32 barterBuyMult { 1.5f };   // the player BUYS at value × this
    f32 barterSellMult { 0.5f };  // the player SELLS at value × this

    REFLECT_BEGIN(StatsTuningForm, data::Form)
        REFLECT_FIELD(attributeToMax)
        REFLECT_FIELD(mitigationPerAttribute)
        REFLECT_FIELD(willBase)
        REFLECT_FIELD(willPerEgo)
        REFLECT_FIELD(basePosture)
        REFLECT_FIELD(posturePerAlacrity)
        REFLECT_FIELD(postureRegenBase)
        REFLECT_FIELD(postureRegenPerAlacrity)
        REFLECT_FIELD(energyRegenBase)
        REFLECT_FIELD(energyRegenPerAlacrity)
        REFLECT_FIELD(healthRegenPerGrace)
        REFLECT_FIELD(essenceRegenBase)
        REFLECT_FIELD(essenceRegenPerInsight)
        REFLECT_FIELD(critSensBase)
        REFLECT_FIELD(critSensPerConstitution)
        REFLECT_FIELD(enduranceBase)
        REFLECT_FIELD(endurancePerAttribute)
        REFLECT_FIELD(statusBuildupDecayFlat)
        REFLECT_FIELD(statusBuildupDecayPercent)
        REFLECT_FIELD(poisonBaseDamagePerSecond)
        REFLECT_FIELD(ignitionDamagePercent)
        REFLECT_FIELD(electrocutionDamagePercent)
        REFLECT_FIELD(electrocutionPostureDrainPercent)
        REFLECT_FIELD(glaciationParalysisDuration)
        REFLECT_FIELD(glaciationEnergyRegenMult)
        REFLECT_FIELD(bleedBurstDamage)
        REFLECT_FIELD(comfortableSleepHours)
        REFLECT_FIELD(sleepPerHour)
        REFLECT_FIELD(flatMitigationCapBase)
        REFLECT_FIELD(staggerSeconds)
        REFLECT_FIELD(survivalThreshold)
        REFLECT_FIELD(survivalResonanceAtEmpty)
        REFLECT_FIELD(hungerHoursPerPoint)
        REFLECT_FIELD(thirstHoursPerPoint)
        REFLECT_FIELD(sleepHoursPerPoint)
        REFLECT_FIELD(barterBuyMult)
        REFLECT_FIELD(barterSellMult)
    REFLECT_END()
};

// Registers the stats Form types (StatsTuningForm). Call at startup.
void registerStatsFormTypes(data::FormTypeRegistry& registry);

// Registers internal gameplay tags needed by the stats system (injury, survival).
// Call once after creating the GameplayTagRegistry at startup.
void registerStatsRuntimeTags(GameplayTagRegistry& tags);

// Resolves the tuning from the database (canonical guid), or defaults if absent.
StatsTuningForm resolveStatsTuning(const data::FormDatabase& forms);

} // namespace gameplay
