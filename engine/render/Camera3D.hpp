#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/core/Defines.hpp"

namespace render {

// Perspective camera for the 3D path. Orientation is yaw/pitch (radians):
// yaw 0 looks down -Z, positive yaw turns right (+X), positive pitch looks up.
// Roll-free on purpose — landscape flying never needs it.
struct Camera3D {
    Vec3 position { 0.0f, 0.0f, 0.0f };
    f32 yaw { 0.0f };
    f32 pitch { 0.0f };
    f32 fovY { glm::radians(60.0f) };
    f32 nearPlane { 0.1f };
    f32 farPlane { 1000.0f };

    Vec3 forward() const {
        const f32 cp = std::cos(pitch);
        return { cp * std::sin(yaw), std::sin(pitch), -cp * std::cos(yaw) };
    }

    Vec3 right() const {
        return glm::normalize(
            glm::cross(forward(), Vec3 { 0.0f, 1.0f, 0.0f }));
    }

    Mat4 view() const {
        return glm::lookAt(position, position + forward(),
                           Vec3 { 0.0f, 1.0f, 0.0f });
    }

    Mat4 proj(f32 aspect) const {
        // REVERSED-Z (docs/RENDERING.md §6.0a): swapping near/far in the
        // 0..1-clip perspective maps near→1, far→0 — float density near
        // zero then gives the FAR field the precision, uniform to the
        // horizon (non-reversed lost ~d²·1.2e-6 m and needed per-case
        // workarounds). Camera path only; shadow/light projections stay
        // non-reversed (ortho depth is linear).
        return glm::perspective(fovY, aspect, farPlane, nearPlane);
    }

    Mat4 viewProj(f32 aspect) const { return proj(aspect) * view(); }
};

} // namespace render
