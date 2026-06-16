#pragma once

#include <vector>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/StatsTuning.hpp"

// Status buildup (docs/STATS.md §3-§4, N1): poison/bleed/mental/disease/curse/
// death accumulate from sources toward a per-type **endurance** threshold; when a
// buildup reaches it, the status triggers — its Status.* tag is granted and the
// buildup resets. Endurance (a derived stat) is the threshold; status-damage (the
// offense side) is how much an attack adds; both come from the stats pipeline.

namespace gameplay {

enum class StatusType { Poison, Bleed, Mental, Disease, Curse, Death };

// Per-type accumulated buildup points (reflected: serializes §5).
struct StatusBuildup {
    f32 poison { 0.0f };
    f32 bleed { 0.0f };
    f32 mental { 0.0f };
    f32 disease { 0.0f };
    f32 curse { 0.0f };
    f32 death { 0.0f };

    REFLECT_BEGIN(StatusBuildup, void)
        REFLECT_FIELD(poison)
        REFLECT_FIELD(bleed)
        REFLECT_FIELD(mental)
        REFLECT_FIELD(disease)
        REFLECT_FIELD(curse)
        REFLECT_FIELD(death)
    REFLECT_END()
};

// Adds buildup points to one status type (clamped at 0).
void addBuildup(StatusBuildup& buildup, StatusType type, f32 points);

// The offense-side scaling (docs/STATS.md §3 "status damage"): the buildup an
// attack inflicts = base × (1 + (attribute − 10) %).
f32 scaledStatusDamage(f32 base, f32 attributeCurrent);

// Decays every buildup; any that reaches its endurance threshold (read from the
// recomputed overlay) triggers: its Status.* tag is granted (ref-counted) and the
// buildup resets. Returns the triggered types (the caller applies their effects).
std::vector<StatusType> tickBuildup(StatusBuildup& buildup, AbilitySystem& system,
                                    f32 dt, const GameplayTagRegistry& tags,
                                    const StatsTuningForm& tuning = {});

// The Status.* tag name for a type (e.g. "Status.Poisoned").
const char* statusTagName(StatusType type);

} // namespace gameplay
