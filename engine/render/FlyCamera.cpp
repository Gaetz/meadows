#include "engine/render/FlyCamera.hpp"

#include "engine/platform/Input.hpp"
#include "engine/platform/Window.hpp"

namespace render {

namespace {
// Just under vertical so forward() never degenerates against the world up.
constexpr f32 kMaxPitch = glm::radians(89.0f);
} // namespace

void FlyCamera::update(platform::Input& input, platform::Window& window,
                       f32 dt, bool allowCapture, LookTrigger trigger) {
    bool wantCapture;
    if (trigger == LookTrigger::Always) {
        // Play-like continuous mouselook (no button), but yield the cursor the
        // instant the caller asks — ImGui hover or a hold-to-free modifier —
        // even mid-capture, so panels stay reachable.
        wantCapture = allowCapture;
    } else {
        const bool rmb = input.mouseDown(platform::MouseButton::Right);
        const bool lmb = input.mouseDown(platform::MouseButton::Left);
        bool held;
        switch (trigger) {
        case LookTrigger::RightButton:
            held = rmb;
            break;
        case LookTrigger::RightOrAltLeft:
            // `captured ||` is what makes the modifier an ARM, not a hold:
            // Alt must be down to START, never to CONTINUE.
            held = rmb ||
                   (lmb && (captured || input.isDown(platform::Key::Alt)));
            break;
        default:
            held = lmb;
            break;
        }
        wantCapture = captured ? held : (held && allowCapture);
    }
    if (wantCapture != captured) {
        captured = wantCapture;
        window.setRelativeMouseMode(captured);
    }

    if (captured) {
        const Vec2 delta = input.mouseDelta();
        camera.yaw += delta.x * lookSensitivity;
        camera.pitch = glm::clamp(camera.pitch - delta.y * lookSensitivity,
                                  -kMaxPitch, kMaxPitch);
    }

    Vec3 move { 0.0f };
    if (input.isDown(platform::Key::W)) { move += camera.forward(); }
    if (input.isDown(platform::Key::S)) { move -= camera.forward(); }
    if (input.isDown(platform::Key::D)) { move += camera.right(); }
    if (input.isDown(platform::Key::A)) { move -= camera.right(); }
    if (input.isDown(platform::Key::E) ||
        input.isDown(platform::Key::Space)) {
        move += Vec3 { 0.0f, 1.0f, 0.0f };
    }
    if (input.isDown(platform::Key::Q) || input.isDown(platform::Key::Ctrl)) {
        move -= Vec3 { 0.0f, 1.0f, 0.0f };
    }
    if (glm::dot(move, move) > 0.0f) {
        const f32 speed = moveSpeed *
                          (input.isDown(platform::Key::Shift) ? fastMultiplier
                                                              : 1.0f);
        camera.position += glm::normalize(move) * speed * dt;
    }
}

} // namespace render
