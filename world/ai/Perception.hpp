#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

// Chantier P0 B2 — actor perception: what an NPC knows about its target.
// Vision = distance + view cone + line of sight (the LOS raycast is the
// CALLER's — same callback pattern as TriggerSystem: world/ never touches
// game/ physics); hearing = OnNoise events routed by the scene. The state
// machine follows the dev rule: enum class + flat switches, EVERY
// transition through setAwareState.
//
// Lives in world/ai (not gameplay/ai): the consumers pair it with the B1
// SpatialIndex and the world Transform — and meadows-world already
// depends on meadows-gameplay, never the reverse.
//
//   Calm ──sight──────────> Alert     (hunting: target position known)
//   Calm ──noise──────────> Suspicious (investigating a position)
//   Alert ──lost too long─> Searching  (going to last known position)
//   Suspicious/Searching ──timeout──> Calm; sight from ANY state → Alert.

namespace world {

enum class AwareState : u8 { Calm, Suspicious, Alert, Searching };

// Reflected component (§2.3): tuning fields are per-actor data an editor
// or a mod can retune; the runtime tail rebuilds after a load.
struct Perception {
    f32 viewDistance { 14.0f };     // sight range (m)
    f32 viewAngleDegrees { 140.0f }; // full width of the vision cone
    f32 hearingRadius { 12.0f };    // OnNoise below this distance is heard
    f32 memorySeconds { 5.0f };     // sight memory before Alert -> Searching
    f32 searchSeconds { 8.0f };     // Searching/Suspicious patience -> Calm
    // Runtime (reflected so a save keeps an alerted camp alerted).
    i32 state { 0 };                // AwareState — reflect has no enum kind
    Vec3 lastKnownPos { 0.0f };     // where the target was last seen/heard
    f32 sinceSeen { 1.0e6f };       // seconds since direct sight
    f32 stateSeconds { 0.0f };      // time in the current state

    REFLECT_BEGIN(Perception, void)
        REFLECT_FIELD(viewDistance)
        REFLECT_FIELD(viewAngleDegrees)
        REFLECT_FIELD(hearingRadius)
        REFLECT_FIELD(memorySeconds)
        REFLECT_FIELD(searchSeconds)
        REFLECT_FIELD(state)
        REFLECT_FIELD(lastKnownPos)
        REFLECT_FIELD(sinceSeen)
        REFLECT_FIELD(stateSeconds)
    REFLECT_END()
};

inline AwareState awareState(const Perception& perception) {
    return static_cast<AwareState>(perception.state);
}

// THE transition function (dev rule) — resets the state clock.
void setAwareState(Perception& perception, AwareState state);

// The vision cone half of `canSee` (horizontal facing, 3D distance).
// The caller ANDs it with its line-of-sight raycast.
bool inViewCone(const Perception& perception, const Vec3& selfPos,
                const Vec3& selfFacing, const Vec3& targetPos);

// Advances one perceiver against one target. `canSee` is this tick's
// full vision verdict (cone AND clear LOS, computed by the caller).
void updatePerception(Perception& perception, bool canSee,
                      const Vec3& targetPos, f32 dt);

// A noise at `position` (heard only within hearingRadius x `loudness`
// of `selfPos` — a sneaking step carries half as far): Calm turns
// Suspicious toward it; Suspicious/Searching re-aim at it; Alert
// doesn't care (it KNOWS where the target is).
void hearNoise(Perception& perception, const Vec3& selfPos,
               const Vec3& position, f32 loudness = 1.0f);

// B3 call-for-help: an ally REPORTED the target at `position` — treated
// as fresh intel (straight to Alert, memory refreshed), no earshot gate:
// the caller's shout already did the range check.
void alertTo(Perception& perception, const Vec3& position);

} // namespace world
