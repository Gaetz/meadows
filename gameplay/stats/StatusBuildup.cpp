#include "gameplay/stats/StatusBuildup.hpp"

#include <algorithm>

namespace gameplay {

namespace {
struct TypeRow {
    f32 StatusBuildup::* field;
    const char* endurance; // the derived threshold stat
    const char* tag;       // the Status.* granted on trigger
};

// Indexed by StatusType. Endurance attribute mapping (docs/STATS.md §3):
// dexterity → poison/bleed, alacrity → mental/disease, perception → curse/death.
constexpr TypeRow kRows[] = {
    { &StatusBuildup::poison, "endurancePoison", "Status.Poisoned" },
    { &StatusBuildup::bleed, "enduranceBleed", "Status.Bleeding" },
    { &StatusBuildup::mental, "enduranceMental", "Status.Mental" },
    { &StatusBuildup::disease, "enduranceDisease", "Status.Diseased" },
    { &StatusBuildup::curse, "enduranceCurse", "Status.Cursed" },
    { &StatusBuildup::death, "enduranceDeath", "Status.Dying" },
};
} // namespace

void addBuildup(StatusBuildup& buildup, StatusType type, f32 points) {
    f32& value = buildup.*kRows[static_cast<int>(type)].field;
    value = std::max(0.0f, value + points);
}

f32 scaledStatusDamage(f32 base, f32 attributeCurrent) {
    return base * (1.0f + (attributeCurrent - 10.0f) / 100.0f);
}

std::vector<StatusType> tickBuildup(StatusBuildup& buildup, AbilitySystem& system,
                                    f32 dt, const GameplayTagRegistry& tags,
                                    const StatsTuningForm& tuning) {
    std::vector<StatusType> triggered;
    for (int i = 0; i < 6; ++i) {
        f32& value = buildup.*kRows[i].field;
        const f32 threshold = currentValueOf(system, attr(kRows[i].endurance));
        if (threshold > 0.0f && value >= threshold) {
            value = 0.0f; // spent to trigger; rebuilds if reinforced
            if (const auto tag = tags.find(kRows[i].tag)) {
                system.tags.add(*tag, tags);
            }
            triggered.push_back(static_cast<StatusType>(i));
        } else {
            value = std::max(0.0f, value - tuning.statusBuildupDecay * dt);
        }
    }
    return triggered;
}

const char* statusTagName(StatusType type) {
    return kRows[static_cast<int>(type)].tag;
}

} // namespace gameplay
