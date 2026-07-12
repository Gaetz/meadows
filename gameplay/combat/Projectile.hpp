#pragma once

#include "engine/core/Defines.hpp"
#include "gameplay/stats/Damage.hpp" // DamageEvent (the captured payload)

// Chantier P0 A7 — projectiles, the sim-pure half: plain ballistics.
// The DIRECTOR (game side) owns collision — a segment raycast against
// the static world per step, and the analytic capsule test against
// actors (CharacterVirtual is outside the broadphase, the A4 lesson).
// The damage payload is CAPTURED at fire time (weaponDamageEvent of the
// shooter) — an arrow in flight doesn't care what its archer does next.

namespace gameplay {

struct Projectile {
    Vec3 position { 0.0f };
    Vec3 velocity { 0.0f };
    f32 gravity { 9.81f };  // m/s² down; 0 = a bolt of light
    f32 ttl { 8.0f };       // seconds of flight before it fizzles
    u64 shooter { 0 };      // entity id — never hits its own archer
    DamageEvent payload;    // captured at fire time
    // Planted (stuck in the world): flight ends, the mesh lingers.
    bool planted { false };
    f32 plantedTtl { 20.0f };
};

// One integration step. Returns the segment START (the previous
// position) — the director sweeps [returned, projectile.position] for
// hits. Planted projectiles only burn their lingering ttl.
Vec3 stepProjectile(Projectile& projectile, f32 dt);

// Done flying AND done lingering — remove it.
bool projectileExpired(const Projectile& projectile);

} // namespace gameplay
