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
    f32 bleedBurstDamage { 30.0f };                // OBSOLETE: bleed now removes
                                                   // criticalSensitivity% of maxHealth (ignores
                                                   // armor). Unused; drop in a data-model pass.
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
    // Offensive stats + combat state machine (chantier 6, appended).
    f32 attackBase { 5.0f };            // attack = base + strength
    f32 critDamageBase { 1.5f };        // crit multiplier base
    // STATS.md says "1.5 + (dexterity−0.5)" — ×7 at dex 6, surely meant
    // per-point scaling. Tunable so the design call stays in data.
    f32 critDamagePerDexterity { 0.05f };
    f32 armorPenPerAlacrity { 0.5f };   // (alacrity−5) × this, floor 0
    f32 resistPenPerInsight { 0.5f };   // (insight−5) × this, floor 0
    f32 shakenThresholdBase { 15.0f };  // shaken if posture hit > (base+con)% of max
    f32 shakenSeconds { 0.6f };         // brief flinch window
    f32 critWindowSeconds { 5.0f };     // posture break → prostrate window
    // Movement & world interaction (audit U4-7, appended): the stat-space
    // -> world mapping (docs/STATS.md §3) and the first-person feel
    // constants, promoted from scattered C++ constexprs so modders can
    // retune them. What deliberately STAYS C++ is tagged [cpp-tuning].
    f32 movementSpeedScale3D { 1.0f / 20.0f }; // movementSpeed stat -> m/s
    f32 sprintMultiplier { 2.0f };             // "sprint multiplies" (STATS.md;
                                               //  dev feel pass 2026-07-11)
    f32 jumpPowerScale3D { 1.0f / 20.8f };     // jumpPower stat -> jump m/s
    f32 accelerationRate3D { 0.12f };          // acceleration stat -> 1/s ramp
    f32 npcWalkFactor { 0.35f };        // NPC walk = jog × this (STATS.md)
    f32 npcPatrolPauseSeconds { 2.5f }; // idle beat at each patrol end
    f32 eyeHeight { 1.7f };             // first-person eye above the feet (m)
    f32 interactionRange { 3.0f };      // [E] prompt reach (m)
    f32 travelFadeSeconds { 0.3f };     // door/travel fade to black (s)
    f32 crimeBountyAssault { 40.0f };   // bounty per witnessed assault
    f32 crimeWitnessRange { 20.0f };    // witness detection radius (m)
    f32 vendorRestockHours { 24.0f };   // vendor inventory restock period
    // Chantier P0 C4a (appended — ordinals stable): the first-person
    // player has no walk anim, so footsteps fire every strideLength
    // meters of grounded travel (NPCs use their clips' AnimEvents).
    f32 strideLength { 2.2f };
    // Chantier P0 A5 (appended): directional blocking. A raised guard
    // catches hits whose attacker stands inside the front cone: caught
    // channels shrink by blockFactor, the blocked amount lands on
    // POSTURE (× blockPostureFactor) — stagger = broken guard.
    f32 blockAngleDegrees { 120.0f }; // full width of the guard cone
    f32 blockFactor { 0.7f };         // caught damage reduced by this
    f32 blockPostureFactor { 0.6f };  // blocked amount -> posture damage
    f32 blockSpeedFactor { 0.5f };    // move speed while guarding
    f32 npcBlockChance { 0.35f };     // odds an NPC guards between swings
    // Chantier P0 B3 (appended): entering Alert shouts — same-faction
    // allies within this radius of the caller join the hunt.
    f32 helpCallRadius { 20.0f };
    // Perfect parry (appended, dev design 2026-07-11): a guard raised at
    // most `window` seconds before the hit negates it entirely and costs
    // the ATTACKER `posture` poise (through applyDamage — stagger =
    // riposte opening).
    f32 perfectParryWindow { 0.2f };
    f32 perfectParryPosture { 10.0f };
    // Dodge (appended, dev design 2026-07-11 — the 2D arena move in 3D):
    // TAPPING sprint (released within dodgeTapSeconds) bursts in the held
    // move direction, backward when none. Speed = jog × the multiplier;
    // the i-frames/cost/cooldown are the Dodge ability's effects.
    f32 dodgeTapSeconds { 0.25f };
    f32 dodgeDurationSeconds { 0.28f }; // match DodgeIFrames.durationSeconds
    f32 dodgeSpeedMultiplier { 2.08f }; // dev feel pass: -20% dodge length
    // STATS.md §4 (appended — ordinals stable): holding the guard is
    // effortful — energy regen halves while State.Blocking is up.
    f32 blockEnergyRegenFactor { 0.5f };

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
        REFLECT_FIELD(attackBase)
        REFLECT_FIELD(critDamageBase)
        REFLECT_FIELD(critDamagePerDexterity)
        REFLECT_FIELD(armorPenPerAlacrity)
        REFLECT_FIELD(resistPenPerInsight)
        REFLECT_FIELD(shakenThresholdBase)
        REFLECT_FIELD(shakenSeconds)
        REFLECT_FIELD(critWindowSeconds)
        REFLECT_FIELD(movementSpeedScale3D)
        REFLECT_FIELD(sprintMultiplier)
        REFLECT_FIELD(jumpPowerScale3D)
        REFLECT_FIELD(accelerationRate3D)
        REFLECT_FIELD(npcWalkFactor)
        REFLECT_FIELD(npcPatrolPauseSeconds)
        REFLECT_FIELD(eyeHeight)
        REFLECT_FIELD(interactionRange)
        REFLECT_FIELD(travelFadeSeconds)
        REFLECT_FIELD(crimeBountyAssault)
        REFLECT_FIELD(crimeWitnessRange)
        REFLECT_FIELD(vendorRestockHours)
        REFLECT_FIELD(strideLength)
        REFLECT_FIELD(blockAngleDegrees)
        REFLECT_FIELD(blockFactor)
        REFLECT_FIELD(blockPostureFactor)
        REFLECT_FIELD(blockSpeedFactor)
        REFLECT_FIELD(npcBlockChance)
        REFLECT_FIELD(helpCallRadius)
        REFLECT_FIELD(perfectParryWindow)
        REFLECT_FIELD(perfectParryPosture)
        REFLECT_FIELD(dodgeTapSeconds)
        REFLECT_FIELD(dodgeDurationSeconds)
        REFLECT_FIELD(dodgeSpeedMultiplier)
        REFLECT_FIELD(blockEnergyRegenFactor)
    REFLECT_END()
};

// Registers the stats Form types (StatsTuningForm). Call at startup.
void registerStatsFormTypes(data::FormTypeRegistry& registry);

// Registers internal gameplay tags needed by the stats system (injury, survival).
// Call once after creating the GameplayTagRegistry at startup.
void registerStatsRuntimeTags(GameplayTagRegistry& tags);

// The FULL runtime tag vocabulary every character-ticking scene needs
// (audit U5-3): life state, the combat statuses, the ten buildup tags,
// exhaustion, plus the stats runtime tags above. One aggregator so a new
// runtime tag can no longer reach one scene and silently miss the others.
// Scene-specific vocabulary (abilities, quest gates, crime) is registered
// on top by each scene (registerTag is idempotent).
void registerCharacterRuntimeTags(GameplayTagRegistry& tags);

// Resolves the tuning from the database (canonical guid), or defaults if absent.
StatsTuningForm resolveStatsTuning(const data::FormDatabase& forms);

} // namespace gameplay
