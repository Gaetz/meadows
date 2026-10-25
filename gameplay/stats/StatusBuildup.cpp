#include "gameplay/stats/StatusBuildup.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>

#include "gameplay/ability/Tags.hpp"

namespace gameplay {

namespace {
// What reaching the threshold does.
enum class OnTrigger : u8 {
    HoldAndTag, // persistent status: buildup held at threshold, tag granted
    Burst,      // one-shot: buildup reset to 0, no tag
};
// The base of the per-second DoT while the status is active (vitality-
// reduced): base × (1 − vit) × dt.
enum class DotSource : u8 {
    None,
    Flat,          // base = dotRate
    MaxHealthPct,  // base = maxHealth × dotRate
    MaxEssencePct, // base = maxEssence × dotRate
};

struct TypeRow {
    f32 StatusBuildup::* field;
    const char* endurance;                  // the derived threshold stat
    std::string_view tag;                   // the Status.* granted on trigger
    OnTrigger onTrigger;
    bool BuildupTickResult::* triggerFlag;  // set when the trigger fires (or null)
    DotSource dotSource;
    f32 StatsTuningForm::* dotRate;         // null when dotSource is None
    f32 BuildupTickResult::* dotOut;        // accumulates the DoT (or null)
};

// dexterity → poison/bleed, alacrity → mental/disease, perception → curse/death.
// Elemental: charisma → ignition (fire), ego → glaciation (cold), insight → electrocution.
// Glaciation has no direct damage: its regen penalty flows through
// buildupStatusModifiers → StatModifiers.
constexpr TypeRow kRows[] = {
    { &StatusBuildup::poison,        "endurancePoison",        tags::Poisoned,
      OnTrigger::HoldAndTag, nullptr,
      DotSource::Flat,          &StatsTuningForm::poisonBaseDamagePerSecond,
      &BuildupTickResult::poisonHealthDamage },                                   // 0
    { &StatusBuildup::bleed,         "enduranceBleed",         tags::Bleeding,
      OnTrigger::Burst,      &BuildupTickResult::bleedBurst,
      DotSource::None,          nullptr, nullptr },                               // 1
    { &StatusBuildup::mental,        "enduranceMental",        tags::Mental,
      OnTrigger::HoldAndTag, nullptr,
      DotSource::None,          nullptr, nullptr },                               // 2
    { &StatusBuildup::disease,       "enduranceDisease",       tags::Diseased,
      OnTrigger::HoldAndTag, nullptr,
      DotSource::None,          nullptr, nullptr },                               // 3
    { &StatusBuildup::curse,         "enduranceCurse",         tags::Cursed,
      OnTrigger::HoldAndTag, nullptr,
      DotSource::None,          nullptr, nullptr },                               // 4
    { &StatusBuildup::death,         "enduranceDeath",         tags::Dying,
      OnTrigger::Burst,      &BuildupTickResult::deathTriggered,
      DotSource::None,          nullptr, nullptr },                               // 5
    { &StatusBuildup::ignition,      "enduranceIgnition",      tags::Ignited,
      OnTrigger::HoldAndTag, nullptr,
      DotSource::MaxHealthPct,  &StatsTuningForm::ignitionDamagePercent,
      &BuildupTickResult::ignitionHealthDamage },                                 // 6
    { &StatusBuildup::glaciation,    "enduranceGlaciation",    tags::Glaciated,
      OnTrigger::HoldAndTag, &BuildupTickResult::glaciationTriggered,
      DotSource::None,          nullptr, nullptr },                               // 7
    { &StatusBuildup::electrocution, "enduranceElectrocution", tags::Electrocuted,
      OnTrigger::HoldAndTag, &BuildupTickResult::electrocutionTriggered,
      DotSource::MaxEssencePct, &StatsTuningForm::electrocutionDamagePercent,
      &BuildupTickResult::electrocutionEssenceDamage },                           // 8
};
constexpr int kTypeCount = static_cast<int>(std::size(kRows));
static_assert(std::size(kRows) == static_cast<size_t>(StatusType::kCount),
              "kRows must mirror StatusType, in enum order");
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
        const TypeRow& row = kRows[i];
        f32& value = buildup.*row.field;
        const auto tag = tags.find(row.tag);
        const bool statusActive = tag && system.tags.has(*tag);
        // Threshold: needed for decay rate (1% of threshold/s) and trigger check.
        const f32 threshold = currentValueOf(system, attr(row.endurance));

        if (statusActive) {
            // Decay at 1% of threshold per second (flat rate → predictable duration).
            const f32 decayPerSec = threshold * tuning.statusBuildupDecayPercent;
            value = std::max(0.0f, value - decayPerSec * dt);

            // Ongoing DoT (applied while buildup is non-zero).
            if (value > 0.0f && row.dotSource != DotSource::None) {
                const f32 rate = tuning.*row.dotRate;
                f32 base = rate;
                switch (row.dotSource) {
                case DotSource::Flat:          break;
                case DotSource::MaxHealthPct:  base = currentValueOf(system, attr("maxHealth")) * rate; break;
                case DotSource::MaxEssencePct: base = currentValueOf(system, attr("maxEssence")) * rate; break;
                case DotSource::None:          break;
                }
                const f32 vit = std::min(currentValueOf(system, attr("vitality")) / 100.0f, 1.0f);
                result.*row.dotOut += base * (1.0f - vit) * dt;
            }

            // Status expires when buildup reaches 0.
            if (value == 0.0f) {
                system.tags.remove(*tag, tags);
            }
        } else {
            if (threshold > 0.0f && value >= threshold) {
                result.triggered.push_back(static_cast<StatusType>(i));

                switch (row.onTrigger) {
                case OnTrigger::Burst:
                    value = 0.0f; // one-shot; no persistent tag
                    if (row.triggerFlag) result.*row.triggerFlag = true;
                    break;
                case OnTrigger::HoldAndTag:
                    value = threshold; // hold at threshold; decays at 1%/s
                    if (row.triggerFlag) result.*row.triggerFlag = true;
                    if (tag) system.tags.add(*tag, tags);
                    break;
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
    const auto check = [&](std::string_view tagName, u32 attrId, f32 mult) {
        const auto t = tags.find(tagName);
        if (t && system.tags.has(*t)) {
            auto [it, ins] = mods.mul.try_emplace(attrId, 1.0f);
            it->second *= mult;
        }
    };
    check(tags::Glaciated,    attr("energyRegen"),  tuning.glaciationEnergyRegenMult);
    check(tags::Electrocuted, attr("essenceRegen"), 0.0f);
}

std::string_view statusTagName(StatusType type) {
    return kRows[static_cast<int>(type)].tag;
}

StatusType parseStatusType(const str& name) {
    return reflect::parseEnum<StatusType>(data::vocab::kStatusTypes, name)
        .value_or(StatusType::Poison);
}

} // namespace gameplay
