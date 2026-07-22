#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp"

// Gameplay volumes (MEADOWS-PLAN §A « Volumes de gameplay », P0): each sim
// tick, actor positions are tested against every loaded TriggerVolume and
// enter/leave transitions fire the volume's data-defined event on the
// EventBus plus its Lua script. The overlap test is an ANALYTIC oriented
// box (position vs halfExtents scaled/rotated by the reference transform)
// — deliberately NOT a Jolt sensor: characters are CharacterVirtual, which
// never enters the broadphase, so Jolt sensors would see nothing that
// matters. If arbitrary rigid bodies ever need to fire triggers, the H3
// facade grows the sensor path then; the observable contract (actor in
// volume → event) stays.
//
// Headless by construction: no renderer, no physics — doctested.

namespace gameplay {
class EventBus;
}

namespace world {

// Runtime occupancy on the trigger entity. NOT reflected: transient state,
// rebuilt as actors move; a cell unload drops it with the entity. (The
// `once` latch, which must survive saves, is TriggerVolume.fired.)
struct TriggerOccupancy {
    vector<u64> inside; // ids of actor entities currently overlapping
};

// The consumer's half: the scene supplies its EventBus and a script
// runner (world/ stays Vm-agnostic — same callback pattern as Collision's
// forEachTriggerOverlap). Either may be empty.
struct TriggerCallbacks {
    gameplay::EventBus* events { nullptr };
    // Runs TriggerVolume.script on ENTER (v1 contract below); actor = who
    // entered.
    std::function<void(const str& script, ecs::Entity actor,
                       ecs::Entity trigger)> runScript;
};

// Advances every loaded trigger volume:
//  - overlap = actor Transform.position inside the trigger's oriented box
//    (halfExtents * trigger scale, rotated by trigger rotation);
//  - ENTER: dispatches Event{ kind = eventKind(volume.event),
//    name = volume.event, source = actor, target = trigger, value = 1 }
//    (skipped when `event` is empty) and runs `script` (skipped when
//    empty);
//  - LEAVE: same event with value = 0; the script does NOT run on leave
//    (v1 — enter-only side effects; revisit if a volume needs teardown);
//  - `once`: the first ENTER runs the callbacks then latches
//    TriggerVolume.fired (reflected → save-persistent); a fired
//    once-trigger never dispatches again.
// Deterministic (§8): triggers iterate in flecs query order; actors in
// flecs query order, or in grid order when `index` is supplied (both
// stable for a given world state).
//
// Pass the frame's SpatialIndex to test only the actors NEAR each
// volume (bounding-sphere radius query) instead of every loaded actor.
// Occupants no longer among the candidates are swept as LEAVE — stepping
// out of the neighborhood IS stepping out of the box. Null = full scan.
class SpatialIndex;
void updateTriggerVolumes(ecs::World& world, const TriggerCallbacks& cb,
                          const SpatialIndex* index = nullptr);

} // namespace world
