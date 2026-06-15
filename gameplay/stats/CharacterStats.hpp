#pragma once

#include "gameplay/ability/DerivedStats.hpp"

namespace ecs {
class World;
}

namespace gameplay {

// Registers the character-stats ECS components (CoreAttributes; later Resonance,
// Survival). Kept separate from registerGameplayComponents so the GAS core
// (gameplay/ability/) stays free of the stats content (gameplay/stats/).
void registerStatsComponents(ecs::World& world);

// Registers the slice's derived-stat calculators: the three primary maxima from
// the nine attributes (×5, docs/STATS.md §1). Later bricks add defense, armor,
// posture, resistances, etc. (same registry).
void registerCoreDerivedStats(DerivedStatRegistry& registry);

} // namespace gameplay
