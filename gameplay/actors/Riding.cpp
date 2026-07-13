#include "gameplay/actors/Riding.hpp"

#include <cmath>

#include <glm/glm.hpp>

namespace gameplay {

RideState stepRide(const RideState& current, const Vec3& wish, f32 speed,
                   f32 accelRate, f32 dt,
                   const std::function<f32(f32, f32)>& groundHeight) {
    RideState next = current;
    // Horizontal only: the ground owns y.
    const Vec3 flat { wish.x, 0.0f, wish.z };
    const Vec3 target = glm::dot(flat, flat) > 0.0f
                            ? glm::normalize(flat) * speed
                            : Vec3 { 0.0f };
    // The player controller's smoothing shape: snappy, never binary.
    next.velocity += (target - next.velocity) *
                     (1.0f - std::exp(-accelRate * dt));
    next.velocity.y = 0.0f;
    next.position.x += next.velocity.x * dt;
    next.position.z += next.velocity.z * dt;
    // Hug the ground with its own smoothing (12/s: fast enough to stick
    // at mount speeds, soft enough that a slope break never pops).
    const f32 ground = groundHeight(next.position.x, next.position.z);
    next.position.y +=
        (ground - next.position.y) * glm::min(1.0f, dt * 12.0f);
    return next;
}

} // namespace gameplay
