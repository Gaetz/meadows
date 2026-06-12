#pragma once

#include <glm/glm.hpp>
#include <SDL3/SDL_events.h>

#include "Defines.h"

namespace graphics {

    class Camera {
    public:
        Vec3 velocity { 0.f };
        Vec3 position { 0.f, 0.f, 5.f };
        float pitch { 0.f };  // Vertical rotation
        float yaw { 0.f };    // Horizontal rotation

        [[nodiscard]] Mat4 getViewMatrix() const;
        [[nodiscard]] Mat4 getRotationMatrix() const;

        void processSDLEvent(const SDL_Event& event);
        void update();
    };

} // namespace graphics
