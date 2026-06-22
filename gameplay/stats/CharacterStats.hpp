#pragma once

#include "gameplay/ability/DerivedStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/StatsTuning.hpp"

namespace ecs {
class World;
}

namespace gameplay {

// Registers the character-stats ECS components (CoreAttributes, Resonance,
// Survival, CombatState). Kept separate from registerGameplayComponents so the
// GAS core (gameplay/ability/) stays free of the stats content (gameplay/stats/).
void registerStatsComponents(ecs::World& world);

// Registers the slice's derived-stat calculators: the three primary maxima
// (docs/STATS.md §1) and the defensive stats (§3 — defense, armor, resistances,
// will, maxPosture, postureRegen, criticalSensitivity). The calculators capture
// `tuning` (§5; defaults match the original constants), so re-register after a
// tuning change.
void registerCoreDerivedStats(DerivedStatRegistry& registry,
                              const StatsTuningForm& tuning = {});

// Recomputes an actor's current values over its CoreAttributes + Vitals, running
// the derived pass and folding in `extra` modifiers (e.g. Resonance). The
// orchestration the scene runs each frame and the execution calcs run after a
// mutation.
void recomputeStats(const CoreAttributes& core, const AttributeSet& vitals,
                    AbilitySystem& system, const DerivedStatRegistry& derived,
                    const StatModifiers* extra = nullptr);

// 3-phase overload: includes Resonance as a third AttrSetRef so GAS effects
// targeting "onyx"/"amber"/"garnet" are picked up. Use for Phase A (extra=null)
// and Phase C (extra=cascade mods) of the character tick.
void recomputeStats(const CoreAttributes& core, const AttributeSet& vitals,
                    const Resonance& resonance,
                    AbilitySystem& system, const DerivedStatRegistry& derived,
                    const StatModifiers* extra = nullptr);

} // namespace gameplay
