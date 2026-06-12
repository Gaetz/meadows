#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// Top-down orthographic camera. `viewHeight` is the number of world units
// visible vertically; the visible width follows the viewport aspect ratio,
// so world scale is resolution-independent.
struct Camera2D {
    Vec2 position { 0.0f, 0.0f }; // world units, view center
    f32 viewHeight { 10.0f };

    Mat4 viewProj(f32 aspect) const {
        const f32 halfH = viewHeight * 0.5f;
        const f32 halfW = halfH * aspect;
        return glm::ortho(position.x - halfW, position.x + halfW,
                          position.y - halfH, position.y + halfH, -1.0f, 1.0f);
    }
};

} // namespace render
