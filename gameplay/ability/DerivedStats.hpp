#pragma once

#include <string_view>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/ability/AbilitySystem.hpp" // AbilitySystem, currentValueOf, attr

// Derived-attribute machinery (generic GAS core, docs/STATS.md §3/§6): a derived
// stat is computed by formula from other attributes, rather than authored. This
// is the piece the Phase-3 GAS deliberately deferred (§6: "custom execution
// calculations only when a concrete case needs them"). Content (the formulas)
// lives in gameplay/stats/; this header is just the mechanism.

namespace gameplay {

// A read view over an AbilitySystem's current-value overlay, passed to derived
// calculators. Sources are read by attribute id (or field name).
struct StatView {
    const AbilitySystem& system;
    f32 get(u32 attrId) const { return currentValueOf(system, attrId); }
    f32 get(std::string_view field) const {
        return currentValueOf(system, attr(field));
    }
};

// A derived attribute: `target` field id = `formula(sources)`. It runs only when
// its `sourceSet` is present on the entity (opt-in) — actors lacking that set
// keep their authored base, so the Phase-3 GAS (no CoreAttributes) is unchanged.
// A non-humanoid / rare item overrides the formula with an infinite Override
// effect (§2.9): the effect modifier wins in the aggregation, so no separate
// override field is needed.
struct DerivedStat {
    u32 target { 0 };                               // attribute field id written
    const reflect::TypeInfo* sourceSet { nullptr }; // required set (null = always)
    f32 (*formula)(const StatView&) { nullptr };
};

class DerivedStatRegistry {
public:
    void add(const DerivedStat& stat) { stats.push_back(stat); }
    const vector<DerivedStat>& all() const { return stats; }

private:
    vector<DerivedStat> stats;
};

// A reflected AttributeSet instance present on an entity (type + pointer), for
// the multi-set current-value recompute (gameplay/ability/GameplayEffects).
struct AttrSetRef {
    const reflect::TypeInfo* type { nullptr };
    const void* instance { nullptr };
};

} // namespace gameplay
