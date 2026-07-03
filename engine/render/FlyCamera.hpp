#pragma once

#include "engine/core/Defines.hpp"
#include "engine/render/Camera3D.hpp"

namespace platform {
class Input;
class Window;
}

namespace render {

// Free-fly camera controller: hold RMB to mouselook (captures the cursor),
// WASD to move along the view direction, E/Space up, Q/Ctrl down, Shift for
// speed boost. Pure input->camera mapping; owns no GPU state.
class FlyCamera {
public:
    Camera3D camera;

    f32 moveSpeed { 20.0f };        // m/s
    f32 fastMultiplier { 5.0f };    // while Shift is held
    f32 lookSensitivity { 0.0025f };// radians per pixel

    void update(platform::Input& input, platform::Window& window, f32 dt);

    bool capturing() const { return captured; }

private:
    bool captured { false };
};

} // namespace render
