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

// Inverse of viewProj for a point: maps a screen pixel (top-left origin) to a
// world position on the camera plane. Flips Y (screen top-left → world +Y up).
inline Vec2 screenToWorld(const Camera2D& camera, Vec2 screenPx,
                          f32 aspect, i32 winW, i32 winH) {
    const Vec2 ndc = {
        (screenPx.x / static_cast<f32>(winW)) * 2.0f - 1.0f,
        1.0f - (screenPx.y / static_cast<f32>(winH)) * 2.0f,
    };
    const f32 halfH = camera.viewHeight * 0.5f;
    const f32 halfW = halfH * aspect;
    return { camera.position.x + ndc.x * halfW,
             camera.position.y + ndc.y * halfH };
}

} // namespace render
