#pragma once

#include <cmath>

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"

namespace ai {

// Perception: is `target` within `range` of `observer` (planar/3D distance)?
// Uses the squared distance to avoid a sqrt.
inline bool withinRange(Vec3 observer, Vec3 target, f32 range) {
    const Vec3 delta = observer - target;
    return (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) <=
           range * range;
}

// Steering: a velocity of magnitude `speed` pointing from `from` toward `to`.
// Zero if they coincide.
inline Vec3 seek(Vec3 from, Vec3 to, f32 speed) {
    const Vec3 delta = to - from;
    const f32 lengthSq =
        delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    if (lengthSq < 1e-8f) {
        return { 0.0f, 0.0f, 0.0f };
    }
    return (delta / std::sqrt(lengthSq)) * speed;
}

} // namespace ai
