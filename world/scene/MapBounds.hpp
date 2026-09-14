#pragma once

#include "engine/core/Defines.hpp"

namespace world {

// The bounded-map sea guard (chantier CARTES M3.3): beyond a map's rim
// the ocean itself turns the swimmer back — an inward current ramping
// with the distance past the rect, composed into the scene's
// water-flow query (the swim drift already applies currents to the
// player; Gothic-style, never a wall, never a kill). Zero inside the
// rect. Ridge-side walking guards come with ridge maps.
inline Vec2 mapBoundsCurrent(f32 x, f32 z, f32 minX, f32 minZ, f32 size,
                             f32 margin, f32 strength) {
    const f32 maxX = minX + size;
    const f32 maxZ = minZ + size;
    Vec2 current { 0.0f, 0.0f };
    if (x < minX) {
        current.x += glm::clamp((minX - x) / margin, 0.0f, 1.0f);
    } else if (x > maxX) {
        current.x -= glm::clamp((x - maxX) / margin, 0.0f, 1.0f);
    }
    if (z < minZ) {
        current.y += glm::clamp((minZ - z) / margin, 0.0f, 1.0f);
    } else if (z > maxZ) {
        current.y -= glm::clamp((z - maxZ) / margin, 0.0f, 1.0f);
    }
    return current * strength;
}

} // namespace world
