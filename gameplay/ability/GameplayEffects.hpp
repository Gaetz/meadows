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
    str attribute2;             // optional second attribute (e.g. affliction attr malus)
    f32 magnitude2 { 0.0f };

    str duration { "instant" }; // "instant" | "duration" | "infinite" | "periodic"
    f32 durationSeconds { 0.0f }; // real-time seconds
    f32 durationHours { 0.0f };   // game-time hours (takes precedence if > 0)
    f32 period { 0.0f };        // periodic interval (seconds)

    str grantedTag;   // granted to the target while a duration/infinite effect lasts
    str requiredTag;  // target must have it (ancestor-aware) to be affected
    str blockedTag;   // target must NOT have it (immunity)

    // Resonance-channel effects (attribute = "onyx"|"amber"|"garnet") only.
    // "immediate" = removed instantly when duration expires.
    // "decay"     = the bonus/penalty fades at decayPerHour pts/game-hour toward 0.
    str expiryMode { "immediate" };
    f32 decayPerHour { 1.0f };
    f32 expiryMagnitude { 0.0f }; // initial decay value on expiry if != 0 (drugs)

    // StatusBuildup routing: if non-empty, routes to tryAddBuildup() instead of
    // a standard attribute effect. Values: "poison", "bleed", "mental", "disease",
    // "curse", "death", "ignition", "glaciation", "electrocution".
    str buildupType;

    // If true, spending energy through this effect does NOT pause energy regen
    // (the post-spend recharge delay). Default false: any energy cost pauses regen
    // for a beat (see updateExhaustion / CharacterTick). Lets some actions cost
    // energy without the recharge penalty.
    bool bypassEnergyRegenDelay { false };

    REFLECT_BEGIN(EffectForm, data::Form)
        REFLECT_FIELD(attribute)
        REFLECT_FIELD(op)
        REFLECT_FIELD(magnitude)
        REFLECT_FIELD(attribute2)
        REFLECT_FIELD(magnitude2)
        REFLECT_FIELD(duration)
        REFLECT_FIELD(durationSeconds)
        REFLECT_FIELD(durationHours)
        REFLECT_FIELD(period)
        REFLECT_FIELD(grantedTag)
        REFLECT_FIELD(requiredTag)
        REFLECT_FIELD(blockedTag)
        REFLECT_FIELD(expiryMode)
        REFLECT_FIELD(decayPerHour)
        REFLECT_FIELD(expiryMagnitude)
        REFLECT_FIELD(buildupType)
        REFLECT_FIELD(bypassEnergyRegenDelay)
    REFLECT_END()
};

struct StatusBuildup; // forward-declare for buildupType routing

// Applies an effect to a target (its AttributeSet + AbilitySystem). Returns
// false if the target's tags fail the effect's required/blocked requirements.
// Instant → BaseValue (+ `damage` meta-attribute routed to health + clamp);
// Duration/Infinite → an active modifier + granted tag, then a CurrentValue
// recompute; Periodic → an active entry that re-applies to BaseValue on tick.
// If effect.durationHours > 0, the effect uses game-time duration (ticked by
// tickGameTimeEffects). If effect.buildupType is non-empty and buildup != nullptr,
// routes to tryAddBuildup() instead. outEffectId receives the allocated effect id
// (0 on failure or for non-tracked effects).
bool applyEffect(AttributeSet& set, AbilitySystem& system,
                 const EffectForm& effect, const GameplayTagRegistry& registry,
                 StatusBuildup* buildup = nullptr, u32* outEffectId = nullptr);

// Advances real-time active effects (gameTime == false) by dt.
void tickEffects(AttributeSet& set, AbilitySystem& system, f32 dt,
                 const GameplayTagRegistry& registry);

// Toggles the shared "State.Exhausted" gate on `system` from current energy,
// with hysteresis: added when energy reaches 0, removed once energy recovers
// above `recoverFraction` of maxEnergy. Energy-costed abilities (dodge, attack,
// sprint) carry blockedTag="State.Exhausted", so tryActivate rejects them while
// it is set. No-op if the tag is not registered. Called once per character tick.
void updateExhaustion(const AttributeSet& set, AbilitySystem& system,
                      const GameplayTagRegistry& registry,
                      f32 recoverFraction = 0.20f);

// Advances game-time active effects (gameTime == true) by gameDt (game-seconds).
void tickGameTimeEffects(AttributeSet& set, AbilitySystem& system, f64 gameDt,
                         const GameplayTagRegistry& registry);

// Removes all active effects whose grantedTag == tag (and drops the tag).
void removeEffectsByGrantedTag(AbilitySystem& system, GameplayTag tag,
                               const GameplayTagRegistry& registry);

// Removes the single active effect with the given effectId (and drops its tag).
void removeEffectById(AbilitySystem& system, u32 effectId,
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
