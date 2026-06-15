#pragma once

#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"
#include "gameplay/ability/AbilitySystem.hpp" // AbilitySystem, currentValueOf, attr

// Derived-attribute machinery (generic GAS core, docs/STATS.md §3/§6): a derived
// stat is computed by formula from other attributes, rather than authored. This
// is the piece the Phase-3 GAS deliberately deferred (§6: "custom execution
// calculations only when a concrete case needs them"). Content (the formulas)
// lives in gameplay/stats/; this header is just the mechanism.

namespace gameplay {

// A reflected AttributeSet instance present on an entity (type + pointer), for
// the multi-set current-value recompute (gameplay/ability/GameplayEffects).
struct AttrSetRef {
    const reflect::TypeInfo* type { nullptr };
    const void* instance { nullptr };
};

// A read view over an entity's attributes, passed to derived calculators. Two
// accessors with a deliberate distinction (docs/STATS.md §2):
//   get()  — the post-modifier CurrentValue (includes the Resonance offset and
//            effects). Secondary stats use this, so they reflect the character's
//            weakened/buffed state.
//   base() — the authored BaseValue (the starting / leveled value). The primary
//            maxima use this, so a temporary attribute change (Resonance, buffs)
//            does NOT move the max — only Resonance's % does; leveling (a base
//            change) still does.
struct StatView {
    const AbilitySystem& system;
    std::span<const AttrSetRef> sets;

    f32 get(u32 attrId) const { return currentValueOf(system, attrId); }
    f32 get(std::string_view field) const { return get(attr(field)); }

    f32 base(u32 attrId) const {
        for (const AttrSetRef& ref : sets) {
            if (!ref.type || !ref.instance) {
                continue;
            }
            if (const reflect::FieldInfo* field = ref.type->findField(attrId);
                field && field->kind == reflect::FieldKind::F32) {
                const reflect::Value value = field->get(ref.instance);
                if (const f32* result = std::get_if<f32>(&value)) {
                    return *result;
                }
            }
        }
        return 0.0f;
    }
    f32 base(std::string_view field) const { return base(attr(field)); }
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
    // A closure so content calculators can capture tuning constants (§5); the
    // generic machinery stays unaware of the stats content.
    std::function<f32(const StatView&)> formula;
};

class DerivedStatRegistry {
public:
    void add(const DerivedStat& stat) { stats.push_back(stat); }
    const vector<DerivedStat>& all() const { return stats; }

private:
    vector<DerivedStat> stats;
};

// Continuous, externally-computed modifiers folded into the recompute alongside
// active effects (e.g. Resonance offsets/scales — gameplay/stats/Resonance, later
// equipment). `add` is summed, `mul` is multiplied — the same semantics as effect
// Add/Multiply, but for non-effect state. Keyed by attribute field id.
struct StatModifiers {
    std::unordered_map<u32, f32> add;
    std::unordered_map<u32, f32> mul;
};

} // namespace gameplay
