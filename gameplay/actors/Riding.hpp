#pragma once

#include <functional>

#include <glm/glm.hpp> // the Followers.hpp precedent: Vec3 by value here

#include "engine/core/Defines.hpp"

// Riding, the sim-pure half: HOW a mounted
// body moves. Same split as Swimming.hpp: the pure step lives
// here so the kinematics are headless-testable; the game-side
// RideController only feeds it input/camera and writes the transforms.

namespace gameplay {

struct RideState {
    Vec3 position { 0.0f }; // the mount's feet on the ground
    Vec3 velocity { 0.0f }; // smoothed horizontal velocity (m/s)
};

// One kinematic step: exponential smoothing of the horizontal velocity
// toward `wish` (a unit-ish direction; zero = coast to a stop) at
// `speed` m/s, integrate x/z, then hug the ground — the height follows
// `groundHeight(x, z)` with its own smoothing so slope changes never
// pop. The same accelerate-by-exp shape as the player controller.
RideState stepRide(const RideState& current, const Vec3& wish, f32 speed,
                   f32 accelRate, f32 dt,
                   const std::function<f32(f32, f32)>& groundHeight);

} // namespace gameplay
