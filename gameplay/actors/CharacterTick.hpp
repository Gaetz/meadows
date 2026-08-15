#pragma once

#include "data/forms/FormDatabase.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity (flecs name confined to meadows-ecs)
#include "engine/core/Defines.hpp"
#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/stats/GameTime.hpp"
#include "gameplay/stats/StatsTuning.hpp"

// tickCharacter — the single reusable character-update function.
//
// All character-state components (CoreAttributes, AttributeSet, AbilitySystem,
// Resonance, Survival, StatusBuildup, CombatState, Injuries, Afflictions,
// ActiveDrugs) live on the flecs entity; this function reads them, runs the
// full per-frame tick pipeline, and writes results back in-place.
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
void tickCharacter(ecs::Entity entity, f32 dt, f64 gameDt,
                   const CharacterTickContext& ctx,
                   const StatModifiers& equipmentMods = {});

// Reset an actor to full vitals (health/energy/essence/posture) based on its
// current mods. Call after spawn or on respawn / "Heal full".
void initializeActorStats(ecs::Entity entity,
                          const CharacterTickContext& ctx,
                          const StatModifiers& equipmentMods = {});

// Assembles the game-time bundle from the entity's components — the same
// set tickCharacter binds; for callers driving the game-time systems
// outside the per-frame tick (sleep, dev time-skips).
GameTimeTickArgs gameTimeArgsFor(ecs::Entity entity,
                                 const CharacterTickContext& ctx);

// Waiting, for one actor entity: gameplay::waitGameTime over its
// components — time passes fully (drug expiry, afflictions, regen,
// injury recovery), only the sleep need is not restored.
GameTimeResult waitCharacter(ecs::Entity entity, GameClock& clock,
                             f32 hours, const CharacterTickContext& ctx,
                             const StatModifiers& equipmentMods = {});

// Sleeping in a bed, for one actor entity: gameplay::sleepGameTime over
// its components — clock jump, the night credited as rest, survival
// decay / regen / drug expiry / injury recovery over the slept window,
// sleep need restored last. The scene passes the resolved equipment mods.
GameTimeResult sleepCharacter(ecs::Entity entity, GameClock& clock,
                              f32 hours, const CharacterTickContext& ctx,
                              const StatModifiers& equipmentMods = {});

} // namespace gameplay
