#pragma once

#include <flecs.h>

#include "data/forms/FormDatabase.hpp"
#include "engine/core/Defines.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/StatsTuning.hpp"

// tickCharacter — the single reusable character-update function for Phase 8+.
//
// All character-state components (CoreAttributes, AttributeSet, AbilitySystem,
// Resonance, Survival, StatusBuildup, CombatState, Injuries, Afflictions,
// ActiveDrugs) live on the flecs entity; this function reads them, runs the
// full per-frame tick pipeline (matching the former manual loop in StatsScene),
// and writes results back in-place.
//
// The pipeline:
//   stagger/paralysis decay → buildCharacterMods → recomputeStats →
//   tickBuildup (DoT + triggers) → posture/energy regen (real-time) →
//   tickGameTime (health/essence regen, survival, drugs, injury recovery)
//
// Shared, non-per-character resources are passed via CharacterTickContext.
// Equipment modifiers are passed separately so the caller resolves inventory.

namespace gameplay {

// World-level resources shared across all character ticks in a scene.
// afflictionDb removed: afflictions/drugs are now GAS game-time effects.
struct CharacterTickContext {
    const DerivedStatRegistry& derived;
    const GameplayTagRegistry& tags;
    const StatsTuningForm&     tuning;
};

// Full real-time + game-time tick for one actor entity.
//   dt      : real-time delta (seconds)
//   gameDt  : game-time delta (game-seconds), already advanced by the caller
//   equipmentMods : precomputed equipment modifiers (caller resolves from inventory)
void tickCharacter(flecs::entity entity, f32 dt, f64 gameDt,
                   const CharacterTickContext& ctx,
                   const StatModifiers& equipmentMods = {});

// Reset an actor to full vitals (health/energy/essence/posture) based on its
// current mods. Call after spawn or on respawn / "Heal full".
void initializeActorStats(flecs::entity entity,
                          const CharacterTickContext& ctx,
                          const StatModifiers& equipmentMods = {});

} // namespace gameplay
