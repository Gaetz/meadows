#pragma once

#include <vector>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/StatsTuning.hpp"

// Status buildup (docs/STATS.md §3-§4): poison/bleed/mental/disease/curse/
// death accumulate from sources toward a per-type **endurance** threshold; when a
// buildup reaches it, the status triggers — its Status.* tag is granted and the
// buildup resets. Endurance (a derived stat) is the threshold; status-damage (the
// offense side) is how much an attack adds; both come from the stats pipeline.

namespace gameplay {

enum class StatusType {
    Poison, Bleed, Mental, Disease, Curse, Death,
    Ignition, Glaciation, Electrocution,
    kCount
};

// Per-type accumulated buildup points (reflected: serializes §5).
struct StatusBuildup {
    f32 poison { 0.0f };
    f32 bleed { 0.0f };
    f32 mental { 0.0f };
    f32 disease { 0.0f };
    f32 curse { 0.0f };
    f32 death { 0.0f };
    f32 ignition { 0.0f };
    f32 glaciation { 0.0f };
    f32 electrocution { 0.0f };

    REFLECT_BEGIN(StatusBuildup, void)
        REFLECT_FIELD(poison)
        REFLECT_FIELD(bleed)
        REFLECT_FIELD(mental)
        REFLECT_FIELD(disease)
        REFLECT_FIELD(curse)
        REFLECT_FIELD(death)
        REFLECT_FIELD(ignition)
        REFLECT_FIELD(glaciation)
        REFLECT_FIELD(electrocution)
    REFLECT_END()
};

// Adds buildup points unconditionally (use in tests or internal code).
void addBuildup(StatusBuildup& buildup, StatusType type, f32 points);

// Adds buildup only if the status is not already active (the "can't re-acquire"
// rule: blocked while the Status.* tag is present and buildup is decreasing).
void tryAddBuildup(StatusBuildup& buildup, StatusType type, f32 points,
                   const AbilitySystem& system, const GameplayTagRegistry& tags);

// The offense-side scaling (docs/STATS.md §3 "status damage"): the buildup an
// attack inflicts = base × (1 + (attribute − 10) %).
f32 scaledStatusDamage(f32 base, f32 attributeCurrent);

// Result of one buildup tick — ongoing effects the caller must apply.
struct BuildupTickResult {
    std::vector<StatusType> triggered;         // types whose status was just acquired
    f32 poisonHealthDamage { 0.0f };           // vitality-reduced HP to remove this tick
    f32 ignitionHealthDamage { 0.0f };         // 0.2%/s of maxHealth while ignited
    f32 electrocutionEssenceDamage { 0.0f };   // 0.2%/s of maxEssence while electrocuted
    bool deathTriggered { false };             // instant kill
    bool bleedBurst { false };                 // critical burst damage (caller applies)
    bool glaciationTriggered { false };        // caller applies 3s paralysis
    bool electrocutionTriggered { false };     // caller collapses posture to 0
};

// Ticks every buildup type:
//   • Before status acquired: decays flat (tuning.statusBuildupDecayFlat/s).
//     At threshold: bleed/death fire instantly + reset; others grant their tag.
//   • After status acquired: decays 1%/s; poison DoT is computed (vitality-
//     reduced, base 1 HP/s); tag is removed when buildup reaches 0.
BuildupTickResult tickBuildup(StatusBuildup& buildup, AbilitySystem& system,
                              f32 dt, const GameplayTagRegistry& tags,
                              const StatsTuningForm& tuning = {});

// Injects regen-rate multipliers for active statuses into mods.mul.
// Call this alongside resonance/injury/drug mods before recomputeStats so that
// currentValueOf(system, attr("essenceRegen")) etc. already reflect suppressions.
// Add one line here per status that affects a regen rate.
void buildupStatusModifiers(const AbilitySystem& system,
                            const GameplayTagRegistry& tags,
                            const StatsTuningForm& tuning,
                            StatModifiers& mods);

// The Status.* tag name for a type (e.g. "Status.Poisoned").
const char* statusTagName(StatusType type);

// Parses a buildup type name from an EffectForm buildupType string.
// Returns StatusType::Poison for unknown strings.
StatusType parseStatusType(const str& name);

} // namespace gameplay
