#pragma once

#include <span>

#include "data/forms/Form.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayTags.hpp"

namespace gameplay {

// A GameplayEffect definition (Form, moddable) — the ONLY way to change an
// attribute (§2.9). Phase 3 keeps it flat: ONE modifier and ONE tag per slot
// (multi-modifier / multi-tag effects compose several effects; lists await the
// reflection container story, Phase 8). Application is a fixed linear pipeline
// (no node-graph); branching is expressed by required/blocked tags.
struct EffectForm : data::Form {
    str attribute;              // target attribute field name ("health", "damage"…)
    str op { "add" };           // "add" | "multiply" | "override"
    f32 magnitude { 0.0f };

    str duration { "instant" }; // "instant" | "duration" | "infinite" | "periodic"
    f32 durationSeconds { 0.0f };
    f32 period { 0.0f };        // periodic interval (seconds)

    str grantedTag;   // granted to the target while a duration/infinite effect lasts
    str requiredTag;  // target must have it (ancestor-aware) to be affected
    str blockedTag;   // target must NOT have it (immunity)

    REFLECT_BEGIN(EffectForm, data::Form)
        REFLECT_FIELD(attribute)
        REFLECT_FIELD(op)
        REFLECT_FIELD(magnitude)
        REFLECT_FIELD(duration)
        REFLECT_FIELD(durationSeconds)
        REFLECT_FIELD(period)
        REFLECT_FIELD(grantedTag)
        REFLECT_FIELD(requiredTag)
        REFLECT_FIELD(blockedTag)
    REFLECT_END()
};

// Applies an effect to a target (its AttributeSet + AbilitySystem). Returns
// false if the target's tags fail the effect's required/blocked requirements.
// Instant → BaseValue (+ `damage` meta-attribute routed to health + clamp);
// Duration/Infinite → an active modifier + granted tag, then a CurrentValue
// recompute; Periodic → an active entry that re-applies to BaseValue on tick.
bool applyEffect(AttributeSet& set, AbilitySystem& system,
                 const EffectForm& effect, const GameplayTagRegistry& registry);

// Advances active effects by dt: periodic ticks (to BaseValue), duration expiry
// (drops granted tags), then a CurrentValue recompute.
void tickEffects(AttributeSet& set, AbilitySystem& system, f32 dt,
                 const GameplayTagRegistry& registry);

// Recomputes every CurrentValue across the given AttributeSets: pass 1 sets each
// non-derived field to (base + Σadd)·Πmult (override wins); pass 2 fills derived
// fields whose source set is present from their formula (then the same modifier
// aggregation); finally vitals are clamped to their current maxima. `derived` may
// be null (no derived pass — the Phase-3 behaviour).
void recomputeCurrent(AbilitySystem& system, std::span<const AttrSetRef> sets,
                      const DerivedStatRegistry* derived = nullptr,
                      const StatModifiers* extra = nullptr);

// Single-set, no-derived overload (the Phase-3 special case): unchanged for the
// existing combat/effects callers.
void recomputeCurrent(const AttributeSet& set, AbilitySystem& system);

} // namespace gameplay
