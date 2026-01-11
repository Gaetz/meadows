#include "Camera.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace graphics {

    Mat4 Camera::getViewMatrix() const {
        // Build translation and rotation matrices
        const Mat4 translation = glm::translate(Mat4(1.f), position);
        const Mat4 rotation = getRotationMatrix();

        // Camera view matrix is the inverse of the camera's world transform
        return glm::inverse(translation * rotation);
    }

    Mat4 Camera::getRotationMatrix() const {
        // Combine pitch (X rotation) and yaw (Y rotation) using quaternions
        const Quat pitchRotation = glm::angleAxis(pitch, Vec3(1.f, 0.f, 0.f));
        const Quat yawRotation = glm::angleAxis(yaw, Vec3(0.f, -1.f, 0.f));

        return glm::toMat4(yawRotation) * glm::toMat4(pitchRotation);
    }

    void Camera::processSDLEvent(const SDL_Event& event) {
        if (event.type == SDL_EVENT_KEY_DOWN) {
            switch (event.key.key) {
                case SDLK_W: velocity.z = -1.f; break;
                case SDLK_S: velocity.z = 1.f; break;
                case SDLK_A: velocity.x = -1.f; break;
                case SDLK_D: velocity.x = 1.f; break;
                case SDLK_Q: velocity.y = -1.f; break;
                case SDLK_E: velocity.y = 1.f; break;
                default: break;
            }
        }

        if (event.type == SDL_EVENT_KEY_UP) {
            switch (event.key.key) {
                case SDLK_W: velocity.z = 0.f; break;
                case SDLK_S: velocity.z = 0.f; break;
                case SDLK_A: velocity.x = 0.f; break;
                case SDLK_D: velocity.x = 0.f; break;
                case SDLK_Q: velocity.y = 0.f; break;
                case SDLK_E: velocity.y = 0.f; break;
                default: break;
            }
        }

        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            // Only rotate when right mouse button is held
            if (event.motion.state & SDL_BUTTON_RMASK) {
                yaw += event.motion.xrel / 200.f;
                pitch -= event.motion.yrel / 200.f;

                // Clamp pitch to prevent camera flipping
                pitch = glm::clamp(pitch, -glm::half_pi<float>() + 0.1f, glm::half_pi<float>() - 0.1f);
            }
        }
    }

    void Camera::update() {
        // Transform velocity by camera rotation to move in the direction we're facing
        const Mat4 rotation = getRotationMatrix();
        const Vec3 movement = Vec3(rotation * Vec4(velocity, 0.f));

        // Apply movement with a speed factor
        constexpr float speed = 0.1f;
        position += movement * speed;
    }

} // namespace graphics
