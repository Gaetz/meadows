#include "engine/terrain/WindField.hpp"

#include <cmath>

namespace render::terrain {

Vec2 windDirectionFromDegrees(f32 degrees) {
    const f32 rad = glm::radians(degrees);
    return { std::cos(rad), -std::sin(rad) };
}

Vec2 WindField::windAt(f32 x, f32 z) const {
    Vec2 wind = globalDir * globalSpeed;
    for (const WindGust& gust : gusts) {
        if (gust.remaining <= 0.0f || gust.radius <= 0.0f) {
            continue;
        }
        const f32 dx = x - gust.x;
        const f32 dz = z - gust.z;
        const f32 q = (dx * dx + dz * dz) / (gust.radius * gust.radius);
        if (q >= 1.0f) {
            continue;
        }
        const f32 falloff = 1.0f - q; // smooth to the edge
        wind += Vec2 { gust.dirX, gust.dirZ } * (gust.speed * falloff);
    }
    return wind;
}

void WindField::tick(f32 dt) {
    for (size_t i = 0; i < gusts.size();) {
        gusts[i].remaining -= dt;
        if (gusts[i].remaining <= 0.0f) {
            gusts.erase(gusts.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

} // namespace render::terrain
