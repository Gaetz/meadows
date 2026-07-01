#include "gameplay/stats/StatusBuildup.hpp"

#include <algorithm>
#include <cstddef>

namespace gameplay {

namespace {
struct TypeRow {
    f32 StatusBuildup::* field;
    const char* endurance; // the derived threshold stat
    const char* tag;       // the Status.* granted on trigger
};

// dexterity → poison/bleed, alacrity → mental/disease, perception → curse/death.
// Elemental: charisma → ignition (fire), ego → glaciation (cold), insight → electrocution.
constexpr TypeRow kRows[] = {
    { &StatusBuildup::poison,        "endurancePoison",        "Status.Poisoned"      }, // 0
    { &StatusBuildup::bleed,         "enduranceBleed",         "Status.Bleeding"      }, // 1
    { &StatusBuildup::mental,        "enduranceMental",        "Status.Mental"        }, // 2
    { &StatusBuildup::disease,       "enduranceDisease",       "Status.Diseased"      }, // 3
    { &StatusBuildup::curse,         "enduranceCurse",         "Status.Cursed"        }, // 4
    { &StatusBuildup::death,         "enduranceDeath",         "Status.Dying"         }, // 5
    { &StatusBuildup::ignition,      "enduranceIgnition",      "Status.Ignited"       }, // 6
    { &StatusBuildup::glaciation,    "enduranceGlaciation",    "Status.Glaciated"     }, // 7
    { &StatusBuildup::electrocution, "enduranceElectrocution", "Status.Electrocuted"  }, // 8
};
constexpr int kTypeCount = static_cast<int>(std::size(kRows));
} // namespace

void addBuildup(StatusBuildup& buildup, StatusType type, f32 points) {
    f32& value = buildup.*kRows[static_cast<int>(type)].field;
    value = std::max(0.0f, value + points);
}

void tryAddBuildup(StatusBuildup& buildup, StatusType type, f32 points,
                   const AbilitySystem& system, const GameplayTagRegistry& tags) {
    const auto tag = tags.find(kRows[static_cast<int>(type)].tag);
    if (tag && system.tags.has(*tag)) return; // blocked while status active
    addBuildup(buildup, type, points);
}

f32 scaledStatusDamage(f32 base, f32 attributeCurrent) {
    return base * (1.0f + (attributeCurrent - 10.0f) / 100.0f);
}

BuildupTickResult tickBuildup(StatusBuildup& buildup, AbilitySystem& system,
                              f32 dt, const GameplayTagRegistry& tags,
                              const StatsTuningForm& tuning) {
    BuildupTickResult result;

    for (int i = 0; i < kTypeCount; ++i) {
        f32& value = buildup.*kRows[i].field;
        const auto tag = tags.find(kRows[i].tag);
        const bool statusActive = tag && system.tags.has(*tag);
        // Threshold: needed for decay rate (1% of threshold/s) and trigger check.
        const f32 threshold = currentValueOf(system, attr(kRows[i].endurance));

        if (statusActive) {
            // Decay at 1% of threshold per second (flat rate → predictable duration).
            const f32 decayPerSec = threshold * tuning.statusBuildupDecayPercent;
            value = std::max(0.0f, value - decayPerSec * dt);

            // Ongoing status effects (applied while buildup is non-zero).
            if (value > 0.0f) {
                const StatusType type = static_cast<StatusType>(i);
                if (type == StatusType::Poison) {
                    const f32 vit = std::min(currentValueOf(system, attr("vitality")) / 100.0f, 1.0f);
                    result.poisonHealthDamage += tuning.poisonBaseDamagePerSecond * (1.0f - vit) * dt;
                } else if (type == StatusType::Ignition) {
                    const f32 maxH = currentValueOf(system, attr("maxHealth"));
                    const f32 will = std::min(currentValueOf(system, attr("will")) / 100.0f, 1.0f);
                    result.ignitionHealthDamage += maxH * tuning.ignitionDamagePercent * (1.0f - will) * dt;
                } else if (type == StatusType::Glaciation) {
                    // regen penalty handled via buildupStatusModifiers → StatModifiers
                } else if (type == StatusType::Electrocution) {
                    const f32 maxE = currentValueOf(system, attr("maxEssence"));
                    const f32 will = std::min(currentValueOf(system, attr("will")) / 100.0f, 1.0f);
                    result.electrocutionEssenceDamage += maxE * tuning.electrocutionDamagePercent * (1.0f - will) * dt;
                }
            }

            // Status expires when buildup reaches 0.
            if (value == 0.0f) {
                system.tags.remove(*tag, tags);
            }
        } else {
            if (threshold > 0.0f && value >= threshold) {
                const StatusType type = static_cast<StatusType>(i);
                result.triggered.push_back(type);

                if (type == StatusType::Bleed) {
                    value = 0.0f;  // one-shot burst; no persistent tag
                    result.bleedBurst = true;
                } else if (type == StatusType::Death) {
                    value = 0.0f;
                    result.deathTriggered = true;
                } else if (type == StatusType::Glaciation) {
                    value = threshold; // hold at threshold; decays at 1%/s
                    result.glaciationTriggered = true;
                    if (tag) system.tags.add(*tag, tags);
                } else if (type == StatusType::Electrocution) {
                    value = threshold;
                    result.electrocutionTriggered = true;
                    if (tag) system.tags.add(*tag, tags);
                } else {
                    // Poison / Mental / Disease / Curse / Ignition: persistent status.
                    value = threshold;
                    if (tag) system.tags.add(*tag, tags);
                }
            } else {
                value = std::max(0.0f, value - tuning.statusBuildupDecayFlat * dt);
            }
        }
    }

    return result;
}

void buildupStatusModifiers(const AbilitySystem& system,
                            const GameplayTagRegistry& tags,
                            const StatsTuningForm& tuning,
                            StatModifiers& mods) {
    const auto check = [&](const char* tagName, u32 attrId, f32 mult) {
        const auto t = tags.find(tagName);
        if (t && system.tags.has(*t)) {
            auto [it, ins] = mods.mul.try_emplace(attrId, 1.0f);
            it->second *= mult;
        }
    };
    check("Status.Glaciated",    attr("energyRegen"),  tuning.glaciationEnergyRegenMult);
    check("Status.Electrocuted", attr("essenceRegen"), 0.0f);
}

const char* statusTagName(StatusType type) {
    return kRows[static_cast<int>(type)].tag;
}

StatusType parseStatusType(const str& name) {
    if (name == "bleed")         return StatusType::Bleed;
    if (name == "mental")        return StatusType::Mental;
    if (name == "disease")       return StatusType::Disease;
    if (name == "curse")         return StatusType::Curse;
    if (name == "death")         return StatusType::Death;
    if (name == "ignition")      return StatusType::Ignition;
    if (name == "glaciation")    return StatusType::Glaciation;
    if (name == "electrocution") return StatusType::Electrocution;
    return StatusType::Poison; // default / "poison"
}

} // namespace gameplay
