#pragma once

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::World, ecs::Entity

namespace gameplay {
class GameplayTagRegistry;
class DerivedStatRegistry;
struct StatsTuningForm;
}

namespace world {

// The kill-z floor.
// Sweeps every actor (Transform + AbilitySystem) whose feet are below
// `killZ` and ends it through the NORMAL pipeline (gameplay::killOutright
// — life state, OnDeath, persistence), never a teleport-back. Actors
// already dead are skipped (a corpse resting below the floor must not
// re-kill and re-log every frame). `exempt` opts one entity out — the
// scene passes the player when god mode is on or outside Play mode.
// Headless; snapshot-then-act (the TriggerSystem discipline): entities
// are collected during the query and killed after it.
void enforceKillZ(ecs::World& world, f32 killZ,
                  const gameplay::GameplayTagRegistry& tags,
                  const gameplay::DerivedStatRegistry& derived,
                  const gameplay::StatsTuningForm& tuning,
                  ecs::Entity exempt = {});

} // namespace world
